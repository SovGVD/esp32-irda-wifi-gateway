#include "drivebackup.h"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdlib>

static bool palm_files(const GoogleDriveFile &f) {
    if (f.folder || f.name.size()<5) return false;
    auto tail=f.name.substr(f.name.size()-4);
    std::transform(tail.begin(),tail.end(),tail.begin(),[](unsigned char c) { return (char)std::tolower(c); });
    return tail==".pdb" || tail==".prc";
}
static bool enumerate(unsigned max,std::vector<HotSync_DBInfo> &dbs,std::string &problem) {
    dbs.clear(); uint32_t start=0;
    while (start<=65535) {
        HotSync_DBInfo db;
        if (!hotsync_read_db_info((uint16_t)start,&db)) {
            if (hotsync_error()==HOTSYNC_DLP_NOT_FOUND) return true;
            problem="Could not list Palm RAM databases (DLP "+std::to_string(hotsync_error())+")"; return false;
        }
        if (dbs.size()>=max) { problem="Palm database count exceeds the configured backup limit"; return false; }
        for (const auto &old:dbs) if (!strcmp(old.name,db.name)) { problem="Palm repeated a database name while listing RAM"; return false; }
        dbs.push_back(db); start=(uint32_t)db.index+1;
    }
    return true;
}
static bool exists(const std::string &name,bool &found,std::string &problem) {
    uint8_t db; found=false;
    if (!hotsync_open_db(name.c_str(),HOTSYNC_MODE_READ|HOTSYNC_MODE_SECRET,&db)) {
        if (hotsync_error()==HOTSYNC_DLP_NOT_FOUND) return true;
        problem="Could not check Palm database "+name+" (DLP "+std::to_string(hotsync_error())+")"; return false;
    }
    found=true;
    if (!hotsync_close_db(db)) { problem="Could not close Palm database "+name; return false; }
    return true;
}
void drivebackup_run(const DriveBackupSetup &setup,DriveBackupHost &host,DriveBackupResult &r) {
    r=DriveBackupResult();
    auto work=[&]() {
        if (!host.online()) { r.problem="Wi-Fi is not connected"; return; }
        DriveBackupState state;
        if (!host.load(state)) { r.problem="Could not load Drive backup state"; return; }
        auto checkpoint=[&]() { if (host.store(state)) return true; r.problem="Could not save Drive backup state. No further files were transferred."; return false; };
        auto clear_pending=[&]() { state.pending=0; state.pending_file.clear(); state.pending_db.clear(); state.recovery_file.clear(); return checkpoint(); };
        uint32_t user; char username[64];
        if (!hotsync_read_user(&user,username,sizeof(username))) { r.problem="Could not read the Palm HotSync user name"; return; }
        r.username=*username ? palm_to_utf8(username,setup.charset) : setup.username;
        if (r.username.empty()) { r.problem="Set a user name in wifi.setup -> HotSync settings, then sync again"; return; }
        if (r.username.size()>128 || r.username.find_first_of("\r\n")!=std::string::npos) { r.problem="Invalid Palm user name"; return; }
        int64_t now=host.now(); HotSync_Time clock;
        if (now<1700000000 || !hotsync_get_time(&clock)) { r.problem="Network/Palm clock is unavailable; cannot safely choose the newest database"; return; }
        uint8_t wall[8]={(uint8_t)(clock.year>>8),(uint8_t)clock.year,clock.month,clock.day,clock.hour,clock.minute,clock.second,0};
        int64_t difference=palm_backup_time(wall,0)-now;
        int timezone=(int)((difference+(difference>=0 ? 450 : -450))/900)*900;
        if (timezone < -12*3600 || timezone > 14*3600 || std::llabs(difference-timezone)>180) {
            r.problem="Set the Palm clock correctly before newest-wins Drive sync"; return;
        }
        std::string token; if (!host.access_token(token,r.problem)) return;
        GoogleDriveClient drive(host.http(),token,setup.max_files,setup.folder_memory); std::string account;
        auto reserve=[&](size_t bytes) {
            if (host.reserve_record_buffer(bytes)) return true;
            r.problem="Not enough memory for Drive restore (largest database block "+std::to_string(bytes)+" bytes). The database was kept.";
            return false;
        };
        if (!drive.root(account)) { r.problem=drive.error(); return; }
        if (!state.account.empty() && state.account==account && state.username==r.username &&
            user==0 && !*username && state.palm_id && setup.restore_saved && !state.pending) {
            // A deliberate full restore to a reset Palm recovers its previous
            // identity too, preserving the other conduits' existing mappings.
            auto old_name=palm_from_utf8(state.username,setup.charset,41);
            if (palm_to_utf8(old_name,setup.charset)!=state.username || !hotsync_write_user(state.palm_id,old_name.c_str())) {
                r.problem="Could not restore the Palm HotSync identity"; return;
            }
            user=state.palm_id;
        }
        if (!state.account.empty() && (state.account!=account || state.palm_id!=user || state.username!=r.username)) {
            r.problem="Google account, Palm HotSync identity or backup user name differs from the saved Drive backup binding"; return;
        }
        if (state.account.empty()) { state.account=account; state.palm_id=user; state.username=r.username; if (!checkpoint()) return; }
        if (!drive.folder(account,"Backup",state.backup_folder,checkpoint) ||
            !drive.folder(state.backup_folder,"Palm",state.palm_folder,checkpoint) ||
            !drive.folder(state.palm_folder,r.username,state.user_folder,checkpoint)) { if (r.problem.empty()) r.problem=drive.error(); return; }
        if (!hotsync_open_conduit()) { r.problem="Could not open the Drive backup conduit"; return; }
        std::vector<HotSync_DBInfo> dbs;
        if (!enumerate(setup.max_databases,dbs,r.problem)) return;
        std::vector<GoogleDriveFile> files;
        if (!drive.list(state.user_folder,files)) { r.problem=drive.error(); return; }
        // A lost upload reply is reconciled by its preassigned file ID, never
        // by sending another create with a new ID.
        if (state.pending=='u') {
            GoogleDriveFile f; bool found;
            if (!drive.get(state.pending_file,f,found)) { r.problem=drive.error(); return; }
            if (found && std::none_of(files.begin(),files.end(),[&](const GoogleDriveFile &p) { return p.id==f.id; })) {
                if (std::find(f.parents.begin(),f.parents.end(),state.user_folder)==f.parents.end()) { r.problem="Pending backup file was moved outside its folder"; return; }
                files.push_back(f);
            }
            if (found && !clear_pending()) return;
        }
        // Validate restore headers and identities before installing any file.
        // Existing managed backups with an unchanged digest need no download.
        std::vector<std::string> names;
        for (auto &f:files) {
            if (!palm_files(f)) { r.ignored++; continue; }
            if (f.managed && !f.backup_md5.empty() && f.md5==f.backup_md5 && !f.db_name.empty()) {
                auto name=palm_from_utf8(f.db_name,setup.charset,31);
                if (palm_to_utf8(name,setup.charset)!=f.db_name) { r.problem="Backup database name cannot be represented on this Palm"; return; }
            } else {
                GoogleDriveReader reader(drive,f); PalmBackupImage image;
                auto read=[&](uint32_t at,uint8_t *p,size_t n) { return reader.read(at,p,n); };
                uint8_t head[78];
                if (f.size<78 || !reader.read(0,head,sizeof(head))) { r.problem="Cannot read Drive Palm file "+f.name+": "+drive.error(); return; }
                // PDB table gets its own cache during validation/restoration.
                reader.table_end(78+(((head[32]<<8)|head[33])&1 ? 10u : 8u)*((head[76]<<8)|head[77]));
                if (!image.open(f.size,read,r.problem)) { r.problem=f.name+": "+r.problem; return; }
                f.db_name=palm_to_utf8(image.info.name,setup.charset);
            }
            if (std::find(names.begin(),names.end(),f.db_name)!=names.end()) { r.problem="Multiple Drive files contain the database "+f.db_name+". Keep one file per Palm database."; return; }
            names.push_back(f.db_name);
        }
        if (setup.backup_only && (state.pending=='r' || state.pending=='s')) { r.problem="An interrupted Drive installation must finish before backup"; return; }
        if (state.pending=='r') {
            bool present;
            if (!exists(state.pending_db,present,r.problem)) return;
            if (present && !setup.retry_interrupted) { r.problem="Interrupted installation of "+state.pending_db+". Use Retry interrupted install on the Drive page before another backup."; return; }
            auto f=std::find_if(files.begin(),files.end(),[&](const GoogleDriveFile &f) { return f.id==state.pending_file; });
            if (f==files.end()) { r.problem="Interrupted install source is missing from Drive"; return; }
            GoogleDriveReader reader(drive,*f); PalmBackupImage image;
            auto read=[&](uint32_t at,uint8_t *p,size_t n) { return reader.read(at,p,n); };
            if (!image.open(f->size,read,r.problem) || state.pending_db!=image.info.name) { if (r.problem.empty()) r.problem="Interrupted install source identifies another database"; return; }
            size_t required=image.record_buffer_size;
            if (!state.recovery_file.empty()) {
                GoogleDriveFile old; bool found;
                if (!drive.get(state.recovery_file,old,found) || !found) { r.problem="Interrupted install recovery copy is unavailable"; return; }
                GoogleDriveReader backup(drive,old); PalmBackupImage rollback;
                auto back=[&](uint32_t at,uint8_t *p,size_t n) { return backup.read(at,p,n); };
                if (!rollback.open(old.size,back,r.problem)) return;
                required=std::max(required,rollback.record_buffer_size);
            }
            if (!reserve(required)) return;
            if (present && !hotsync_delete_db(state.pending_db.c_str())) { r.problem="Could not remove the interrupted installation"; return; }
            // Preserve the source ID until its installation is retried below.
        }
        if (state.pending=='r' || state.pending=='s') {
            auto source=std::find_if(files.begin(),files.end(),[&](const GoogleDriveFile &f) { return f.id==state.pending_file; });
            if (source==files.end()) { r.problem="Interrupted transfer source is missing from Drive"; return; }
            std::rotate(files.begin(),source,source+1);
        }
        for (auto &f:files) {
            if (setup.backup_only) break;
            if (!palm_files(f)) continue;
            auto name=palm_from_utf8(f.db_name,setup.charset,31); bool present;
            if (!exists(name,present,r.problem)) return;
            bool pending=state.pending=='r' && state.pending_file==f.id;
            bool saving=state.pending=='s' && state.pending_file==f.id;
            if (state.pending=='u') continue; // Finish a reserved upload first.
            if (!present && f.managed && !setup.restore_saved && !pending) { r.retained++; continue; }
            auto existing=std::find_if(dbs.begin(),dbs.end(),[&](const HotSync_DBInfo &db) { return name==db.name; });
            if (present && existing==dbs.end()) { r.retained++; continue; } // ROM is never replaced.
            if (present) {
                PalmBackupExport previous(*existing);
                if (!previous.prepare()) { r.problem=previous.error(); return; }
                if (f.managed && f.signature==previous.signature() && !f.backup_md5.empty() && f.md5==f.backup_md5) {
                    if (!previous.close()) r.problem=previous.error();
                    if (saving && r.problem.empty()) clear_pending();
                    if (!r.problem.empty()) return;
                    continue;
                }
                int64_t modified=palm_backup_time(existing->modified,timezone);
                if (!modified || !f.modified) { r.problem="Cannot compare modification times for "+name; return; }
                if (f.modified<=modified) {
                    if (!previous.close()) r.problem=previous.error();
                    if (saving && r.problem.empty()) clear_pending();
                    if (!r.problem.empty()) return;
                    continue;
                }
                // Verify the incoming pinned revision before preserving/deleting
                // any existing Palm database. Only a confirmed recovery upload
                // permits the replacement below.
                GoogleDriveReader check(drive,f); PalmBackupImage image;
                auto read=[&](uint32_t at,uint8_t *p,size_t n) { return check.read(at,p,n); };
                if (!image.open(f.size,read,r.problem) || name!=image.info.name) { if (r.problem.empty()) r.problem="Drive database identity changed"; return; }
                if (!reserve(std::max(image.record_buffer_size,previous.record_buffer_size()))) return;
                if (state.pending && !saving) { r.problem="A previous Drive transfer must finish first"; return; }
                state.pending='s'; state.pending_file=f.id; state.pending_db=name;
                if (!checkpoint() || !drive.folder(state.user_folder,"Recovery",state.recovery_folder,checkpoint)) { if (r.problem.empty()) r.problem=drive.error(); return; }
                if (state.recovery_file.empty() && (!drive.generate_id(state.recovery_file) || !checkpoint())) { if (r.problem.empty()) r.problem=drive.error(); return; }
                GoogleDriveFile target,saved; bool found;
                if (!drive.get(state.recovery_file,target,found)) { r.problem=drive.error(); return; }
                if (!found) target.id=state.recovery_file;
                std::string filename=f.db_name+"."+std::to_string(modified)+"."+previous.signature()+((existing->flags&1) ? ".prc" : ".pdb");
                host.trace("Saving previous Palm version of "+name);
                if (!drive.upload(target,!found,state.recovery_folder,filename,f.db_name,previous.signature(),modified,previous.size(),
                    [&](uint8_t *p,size_t n) { return previous.read(p,n); },saved,"application/octet-stream","",[&]() { return previous.rewind(); }) || !previous.close()) {
                    r.problem=previous.error().empty() ? drive.error() : previous.error(); return;
                }
                state.pending='r'; if (!checkpoint()) return;
                if (!hotsync_delete_db(name.c_str())) { r.problem="Could not replace "+name+". Previous copy is safe in Recovery."; return; }
                pending=true;
            }
            GoogleDriveReader reader(drive,f); PalmBackupImage image;
            auto read=[&](uint32_t at,uint8_t *p,size_t n) { return reader.read(at,p,n); };
            if (!image.open(f.size,read,r.problem)) return;
            if (!reserve(image.record_buffer_size)) return;
            reader.table_end(78+image.count*((image.info.flags&1) ? 10u : 8u));
            if (state.pending && !pending) { r.problem="A previous backup transfer must finish before installing Drive files"; return; }
            state.pending='r'; state.pending_file=f.id; state.pending_db=image.info.name;
            if (!checkpoint()) return;
            if (f.modified) palm_backup_modified(image.info,f.modified,timezone);
            host.trace("Installing "+f.name);
            if (!palm_backup_install(image,read,r.problem)) {
                if (!drive.error().empty()) r.problem+=". "+drive.error();
                std::string first=r.problem;
                if (hotsync_error()!=HOTSYNC_ERR_LINK && hotsync_delete_db(image.info.name)) {
                    bool recovered=state.recovery_file.empty();
                    if (!recovered) {
                        GoogleDriveFile old; bool found;
                        if (drive.get(state.recovery_file,old,found) && found) {
                            GoogleDriveReader backup(drive,old); PalmBackupImage rollback;
                            auto back=[&](uint32_t at,uint8_t *p,size_t n) { return backup.read(at,p,n); };
                            std::string error;
                            recovered=rollback.open(old.size,back,error) && name==rollback.info.name && palm_backup_install(rollback,back,error);
                        }
                    }
                    if (recovered) { if (clear_pending()) r.problem=first; }
                    else r.problem=first+". Previous Palm version remains in Recovery; retry the interrupted install.";
                }
                return;
            }
            if (!clear_pending()) return;
            r.restored++; if (image.info.flags&0x20) r.reset_needed=true;
        }
        if (state.pending=='r') { r.problem="Interrupted install source was not available to retry"; return; }
        if (setup.restore_only) return;
        // Restored databases are included in the same complete RAM backup.
        if (r.restored && !enumerate(setup.max_databases,dbs,r.problem)) return;
        if (state.pending=='u') {
            auto pending=std::find_if(dbs.begin(),dbs.end(),[&](const HotSync_DBInfo &db) { return state.pending_db==db.name; });
            if (pending==dbs.end()) { r.problem="Pending Drive upload's database is no longer on the Palm"; return; }
            std::rotate(dbs.begin(),pending,pending+1);
        }
        for (const auto &db:dbs) {
            if (db.flags&0x7080u) { r.problem=std::string(db.name)+": unsupported stream/extended database format"; return; }
            std::string name=palm_to_utf8(db.name,setup.charset),filename=name+((db.flags&1) ? ".prc" : ".pdb");
            auto file=files.end();
            for (auto f=files.begin();f!=files.end();++f) if (palm_files(*f) && (f->db_name==name || f->name==filename)) {
                if (file!=files.end()) { r.problem="Multiple Drive targets for "+filename; return; } file=f;
            }
            PalmBackupExport source(db);
            if (!source.prepare()) { r.problem=filename+": "+source.error(); return; }
            std::string signature=source.signature();
            bool resume=state.pending=='u' && state.pending_db==db.name;
            if (file!=files.end() && file->managed && file->signature==signature && !file->backup_md5.empty() && file->backup_md5==file->md5 && file->size==source.size() && !resume) {
                if (!source.close()) { r.problem=source.error(); return; }
                r.unchanged++; continue;
            }
            int64_t modified=palm_backup_time(db.modified,timezone);
            if (file!=files.end() && (!modified || !file->modified || file->modified>modified)) {
                r.problem="Drive copy of "+filename+" changed since restore. Run HotSync again to choose the newest version."; return;
            }
            if (state.pending && !resume) { r.problem="A pending Drive backup refers to a missing/different Palm database"; return; }
            GoogleDriveFile target; bool create=file==files.end();
            if (!create) target=*file;
            else if (resume) target.id=state.pending_file;
            else if (!drive.generate_id(target.id)) { r.problem=drive.error(); return; }
            state.pending='u'; state.pending_file=target.id; state.pending_db=db.name;
            if (!checkpoint()) return;
            host.trace("Backing up "+filename);
            GoogleDriveFile saved;
            if (!drive.upload(target,create,state.user_folder,filename,name,signature,(palm_backup_time(db.modified,timezone) ? palm_backup_time(db.modified,timezone) : now),source.size(),
                [&](uint8_t *p,size_t n) { return source.read(p,n); },saved,"application/octet-stream","",[&]() { return source.rewind(); })) {
                r.problem=filename+": "+(source.error().empty() ? drive.error() : source.error()); return;
            }
            if (!source.close()) { r.problem=source.error(); return; }
            if (!clear_pending()) return;
            r.uploaded++;
        }
        if (state.pending) r.problem="A pending backup could not be reconciled";
    };
    work(); r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK;
}
std::string drivebackup_summary(const DriveBackupResult &r) {
    std::string s=std::to_string(r.uploaded)+" backed up, "+std::to_string(r.restored)+" installed/restored, "+std::to_string(r.unchanged)+" unchanged";
    if (r.retained) s+=", "+std::to_string(r.retained)+" saved backups retained";
    if (r.reset_needed) s+=". Reset the Palm after HotSync to activate installed updates";
    if (!r.problem.empty()) s+=". "+r.problem;
    return s;
}
