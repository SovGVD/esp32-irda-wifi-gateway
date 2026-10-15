#include "notesync.h"
#include "cJSON.h"
#include <memory>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cctype>
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
static std::string field(const cJSON *j,const char *name) { auto *p=cJSON_GetObjectItemCaseSensitive(j,name); return cJSON_IsString(p) ? p->valuestring : ""; }
static bool number(const cJSON *p,uint32_t max,uint32_t &out) { if (!cJSON_IsNumber(p) || p->valuedouble<0 || p->valuedouble>max || floor(p->valuedouble)!=p->valuedouble) return false; out=(uint32_t)p->valuedouble; return true; }
bool notesync_encode(const NoteSyncState &s,std::string &data) {
    Json j(cJSON_CreateObject(),cJSON_Delete); if (!j) return false;
    bool ok=cJSON_AddNumberToObject(j.get(),"v",1) && cJSON_AddNumberToObject(j.get(),"palm",s.palm_id);
    const char *keys[]={"account","user","root","parent","folder"};
    const std::string *values[]={&s.account,&s.username,&s.root_folder,&s.user_folder,&s.folder};
    for (unsigned i=0;i<5;i++) if (!cJSON_AddStringToObject(j.get(),keys[i],values[i]->c_str())) ok=false;
    auto *map=cJSON_AddArrayToObject(j.get(),"map"); if (!map) return false;
    for (const auto &m:s.map) {
        auto *item=cJSON_CreateObject(); if (!item) return false;
        if (!cJSON_AddItemToArray(map,item)) { cJSON_Delete(item); return false; }
        if (!cJSON_AddNumberToObject(item,"p",m.palm_id) || !cJSON_AddStringToObject(item,"g",m.google_id.c_str()) || !cJSON_AddStringToObject(item,"m",m.marker.c_str()) ||
            !cJSON_AddNumberToObject(item,"s",m.pending) || !cJSON_AddNumberToObject(item,"a",m.fields[0]) || !cJSON_AddNumberToObject(item,"b",m.fields[1])) ok=false;
    }
    if (!ok) return false;
    char *raw=cJSON_PrintUnformatted(j.get()); if (!raw) return false; data=raw; free(raw); return data.size()<=16000;
}
bool notesync_decode(const std::string &data,unsigned max,NoteSyncState &s) {
    s=NoteSyncState(); if (data.size()>16000) return false;
    Json j(cJSON_ParseWithOpts(data.c_str(),nullptr,true),cJSON_Delete); uint32_t version;
    if (!cJSON_IsObject(j.get()) || !number(cJSON_GetObjectItemCaseSensitive(j.get(),"v"),1,version) || version!=1 || !number(cJSON_GetObjectItemCaseSensitive(j.get(),"palm"),UINT32_MAX,s.palm_id)) return false;
    const char *keys[]={"account","user","root","parent","folder"}; std::string *values[]={&s.account,&s.username,&s.root_folder,&s.user_folder,&s.folder};
    for (unsigned i=0;i<5;i++) { if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j.get(),keys[i]))) return false; *values[i]=field(j.get(),keys[i]); if (values[i]->size()>256) return false; }
    auto valid_id=[](const std::string &id) { return id.size()<=256 && id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos; };
    if (s.account.empty() || s.username.empty() || !valid_id(s.account) || !valid_id(s.root_folder) || !valid_id(s.user_folder) || !valid_id(s.folder)) return false;
    auto *map=cJSON_GetObjectItemCaseSensitive(j.get(),"map"); if (!cJSON_IsArray(map) || (unsigned)cJSON_GetArraySize(map)>max) return false;
    const cJSON *item; cJSON_ArrayForEach(item,map) {
        NoteSyncMap m; uint32_t pending;
        if (!number(cJSON_GetObjectItemCaseSensitive(item,"p"),0xffffff,m.palm_id) || !m.palm_id ||
            !number(cJSON_GetObjectItemCaseSensitive(item,"s"),3,pending) || !number(cJSON_GetObjectItemCaseSensitive(item,"a"),UINT32_MAX,m.fields[0]) || !number(cJSON_GetObjectItemCaseSensitive(item,"b"),UINT32_MAX,m.fields[1])) return false;
        m.pending=pending; m.google_id=field(item,"g"); m.marker=field(item,"m");
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item,"g")) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item,"m"))) return false;
        if (!valid_id(m.google_id) || m.marker.size()>100 || (m.google_id.empty() && (!m.pending || m.pending==3)) || (m.pending && m.pending!=3 && m.marker.empty()) || s.folder.empty()) return false;
        for (const auto &old:s.map) if (old.palm_id==m.palm_id || (!m.google_id.empty() && old.google_id==m.google_id) || (!m.marker.empty() && old.marker==m.marker)) return false;
        s.map.push_back(m);
    }
    return true;
}
struct Local { uint32_t id; uint8_t attr,category; PalmNote note; std::array<uint32_t,2> fields; };
// Keep only compact fingerprints between records. Scope guards also release
// reloaded bitmaps on conflicts, interrupted operations and early returns.
struct BitmapRelease {
    PalmNote *note;
    explicit BitmapRelease(PalmNote *n) : note(n) {}
    ~BitmapRelease() { if (note) std::string().swap(note->pixels); }
};
bool notesync_retry_unconfirmed(NoteSyncState &state) {
    bool changed=false;
    for (auto &m:state.map) if (m.pending==2 && m.google_id.empty()) { m.pending=1; changed=true; }
    return changed;
}
struct Remote { GoogleNote cloud; PalmNote display; std::array<uint32_t,2> fields={}; bool missing=false,moved=false; };
static bool accepted(const GoogleDriveFile &f,bool drawing) {
    if (f.folder || f.trashed) return false;
    if (!drawing) return f.mime=="application/vnd.google-apps.document";
    if (f.name.size()<4) return false;
    std::string suffix=f.name.substr(f.name.size()-4); std::transform(suffix.begin(),suffix.end(),suffix.begin(),[](unsigned char c) { return (char)std::tolower(c); });
    return suffix==".png" && (f.mime=="image/png" || f.mime=="application/octet-stream");
}
static bool project(GoogleNote &cloud,const NoteSyncSetup &setup,PalmNote &n,std::string &problem) {
    // Drawings need only one bitmap: their Google representation is already
    // monochrome. Text keeps the original Unicode for field-preserving edits.
    if (setup.drawing) n=std::move(cloud.note); else n=cloud.note;
    if (setup.drawing) n.title=palm_from_utf8(n.title,setup.charset,256);
    else n.text=palm_from_utf8(n.text,setup.charset,4096);
    if (n.title.size()>255 || n.text.size()>4095) { problem=cloud.file.name+": text exceeds the classic Palm record limit"; return false; }
    return true;
}
static bool write_palm(NoteSyncHost &host,uint8_t db,const Local &local,const PalmNote &note,bool drawing,const HotSync_Time &now,std::string &problem) {
    size_t bytes=drawing ? drawing_record_size(note) : note.text.size()+1;
    if (!bytes) { problem="Note cannot be encoded within the Palm record limits"; return false; }
    if (!host.reserve_record_buffer(bytes)) {
        problem="Not enough HotSync buffer to write the note ("+std::to_string(bytes)+" bytes)"; return false;
    }
    size_t room; uint8_t *p=hotsync_record_buffer(&room); uint32_t id;
    size_t n=drawing ? drawing_pack(note,now,p,room) : memo_pack(note,p,room);
    if (!n || !hotsync_write_record(db,local.id,local.attr&~HOTSYNC_ATTR_DIRTY,local.category,n,&id) || id!=local.id) return false;
    if (!host.release_record_buffer()) { problem="Could not release the notes record buffer"; return false; }
    return true;
}
static bool reload_drawing(NoteSyncHost &host,uint8_t db,const Local &snapshot,PalmNote &note,std::string &problem) {
    HotSync_Record rec;
    if (!hotsync_read_record(db,snapshot.id,&rec)) { problem="Could not reread the Palm drawing after validation"; return false; }
    if (rec.truncated) {
        size_t length=rec.total_len;
        if (length>PALM_NOTE_RECORD_MAX || !host.reserve_record_buffer(length)) { problem="Not enough HotSync buffer to reread the Palm drawing"; return false; }
        if (!hotsync_read_record(db,snapshot.id,&rec)) { problem="Could not reread the complete Palm drawing"; return false; }
    }
    if (rec.truncated || rec.id!=snapshot.id || rec.attr!=snapshot.attr || rec.category!=snapshot.category ||
        !drawing_unpack(rec.data,rec.len,note,problem)) {
        if (problem.empty()) problem="Palm drawing changed or became invalid after validation";
        return false;
    }
    if (note_fields(note,true)!=snapshot.fields || note.header!=snapshot.note.header || note.body_word!=snapshot.note.body_word) {
        problem="Palm drawing changed after validation; sync again"; return false;
    }
    if (!host.release_record_buffer()) { problem="Could not release the notes record buffer after rereading"; return false; }
    return true;
}
void notesync_run(const NoteSyncSetup &setup,NoteSyncHost &host,NoteSyncResult &r) {
    r=NoteSyncResult(); uint8_t db=0; bool opened=false;
    auto work=[&]() {
        if (!host.online()) { r.problem="Wi-Fi is not connected"; return; }
        if (!setup.max_notes || setup.max_notes>512 || setup.conflicts>2) { r.problem="Invalid notes sync limits"; return; }
        NoteSyncState state;
        if (!host.load(state) || state.map.size()>setup.max_notes) { r.problem="Could not load notes sync state"; return; }
        auto checkpoint=[&]() { if (host.store(state)) return true; r.problem="Could not save notes sync state; no further transfers were made"; return false; };
        uint32_t user; char username[128];
        if (!hotsync_read_user(&user,username,sizeof(username))) { r.problem="Could not read Palm HotSync identity"; return; }
        std::string name=palm_to_utf8(username,setup.charset);
        if (name.empty()) { r.problem="Set a user name in wifi.setup -> HotSync settings, then sync again"; return; }
        HotSync_Time now={}; if (!hotsync_get_time(&now) || host.now()<1700000000) { r.problem="Palm/network clock is unavailable"; return; }
        std::string token; if (!host.access_token(token,r.problem)) return;
        GoogleNotesClient google(host.http(),token,setup.max_notes+8); std::string account;
        if (!google.drive.root(account)) { r.problem=google.drive.error(); return; }
        if (!state.account.empty() && (state.account!=account || state.palm_id!=user || state.username!=name)) { r.problem="Google account or Palm HotSync identity differs from the saved notes binding"; return; }
        if (state.account.empty()) { state.account=account; state.palm_id=user; state.username=name; if (!checkpoint()) return; }
        if (!google.drive.folder(account,"Palm",state.root_folder,checkpoint) || !google.drive.folder(state.root_folder,name,state.user_folder,checkpoint) ||
            !google.drive.folder(state.user_folder,setup.drawing ? "Notes" : "Memos",state.folder,checkpoint)) { if (r.problem.empty()) r.problem=google.drive.error(); return; }
        if (!hotsync_open_conduit() || !hotsync_open_db(setup.drawing ? "npadDB" : "MemoDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE|HOTSYNC_MODE_SECRET,&db)) {
            r.problem=setup.drawing ? "Could not open npadDB. Standard Palm OS 4 Note Pad is required." : "Could not open MemoDB"; return;
        }
        opened=true;
        std::vector<Local> local; size_t bytes=0,largest_bitmap=0;
        for (unsigned i=0;i<=setup.max_notes;i++) {
            HotSync_Record rec;
            if (!hotsync_read_record_by_index(db,i,&rec)) { if (hotsync_error()==HOTSYNC_DLP_NOT_FOUND) break; r.problem="Could not read all Palm notes"; return; }
            if (i==setup.max_notes) { r.problem="Palm notes exceed the configured record limit"; return; }
            Local l={}; l.id=rec.id; l.attr=rec.attr; l.category=rec.category;
            if (!l.id || l.id>0xffffff || l.category>15 || std::any_of(local.begin(),local.end(),[&](const Local &v) { return v.id==l.id; })) { r.problem="Invalid or repeated Palm note identity"; return; }
            if (!(l.attr&(HOTSYNC_ATTR_DELETED|HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED))) {
                if (setup.drawing && rec.truncated) {
                    size_t length=rec.total_len;
                    if (length>PALM_NOTE_RECORD_MAX || !host.reserve_record_buffer(length)) {
                        r.problem="Not enough HotSync buffer for Note Pad record "+std::to_string(i)+" ("+std::to_string(length)+" bytes)"; return;
                    }
                    if (!hotsync_read_record_by_index(db,i,&rec) || rec.id!=l.id || rec.attr!=l.attr || rec.category!=l.category || rec.total_len!=length) {
                        r.problem="Note Pad record changed while reading the complete drawing"; return;
                    }
                }
                if (rec.truncated || !(setup.drawing ? drawing_unpack(rec.data,rec.len,l.note,r.problem) : memo_unpack(rec.data,rec.len,l.note,r.problem))) {
                    r.problem="Palm record "+std::to_string(i)+" (ID "+std::to_string(l.id)+", "+std::to_string(rec.len)+
                        " received bytes): "+(r.problem.empty() ? "record was truncated" : r.problem); return;
                }
                l.fields=note_fields(l.note,setup.drawing);
                largest_bitmap=std::max(largest_bitmap,l.note.pixels.size());
                bytes+=l.note.text.size()+l.note.title.size()+l.note.header.size();
                if (bytes+l.note.pixels.size()>setup.content_memory) { r.problem="Palm notes exceed the content memory budget"; return; }
                if (setup.drawing) std::string().swap(l.note.pixels);
            }
            local.push_back(std::move(l));
        }
        if (!host.release_record_buffer()) { r.problem="Could not release the notes record buffer before Google access"; return; }
        std::vector<GoogleDriveFile> files;
        if (!google.drive.list(state.folder,files)) { r.problem=google.drive.error(); return; }
        for (const auto &f:files) if (!f.sync_marker.empty()) {
            if (std::count_if(files.begin(),files.end(),[&](const GoogleDriveFile &v) { return v.sync_marker==f.sync_marker; })!=1) { r.problem="Duplicate Google note creation markers; keep one file per linked note"; return; }
        }
        // Reconcile Docs creates by a persisted private marker. An ambiguous
        // create is never automatically sent again with a fresh file identity.
        for (auto &m:state.map) if ((m.pending==1 || m.pending==2) && m.google_id.empty()) {
            auto f=std::find_if(files.begin(),files.end(),[&](const GoogleDriveFile &v) { return v.sync_marker==m.marker; });
            if (f!=files.end()) { m.google_id=f->id; m.pending=1; if (!checkpoint()) return; }
            else if (m.pending==2) {
                r.problem="A Google memo creation is unconfirmed. Sync again later, or use Retry unconfirmed creations after checking Drive."; return;
            }
        }
        std::vector<Remote> remote;
        // Confirm absence of linked files individually, including moved files.
        for (const auto &m:state.map) if (!m.google_id.empty() && std::none_of(files.begin(),files.end(),[&](const GoogleDriveFile &f) { return f.id==m.google_id; })) {
            GoogleDriveFile f; bool found;
            if (!google.drive.get(m.google_id,f,found)) { r.problem=google.drive.error(); return; }
            if (!found || f.trashed) { if (m.pending==1) continue; Remote item; item.cloud.file.id=m.google_id; item.missing=true; remote.push_back(item); }
            else if (std::find(f.parents.begin(),f.parents.end(),state.folder)==f.parents.end()) { Remote item; item.cloud.file=f; item.moved=true; remote.push_back(item); }
            else { files.push_back(f); }
        }
        for (const auto &f:files) {
            bool linked=std::any_of(state.map.begin(),state.map.end(),[&](const NoteSyncMap &m) { return m.google_id==f.id; });
            if (!accepted(f,setup.drawing)) { if (linked) { r.problem="A linked Google note changed to an unsupported file format"; return; } r.skipped++; continue; }
            Remote item;
            if (!google.read(f,setup.drawing,item.cloud)) { r.problem=google.error(); return; }
            size_t original_bytes=item.cloud.note.text.size()+item.cloud.note.title.size()+item.cloud.note.pixels.size();
            if (!project(item.cloud,setup,item.display,r.problem)) return;
            item.fields=note_fields(item.display,setup.drawing);
            largest_bitmap=std::max(largest_bitmap,item.display.pixels.size());
            // Memos retain Unicode and projected text. Drawings retain only
            // metadata/hashes, with one bitmap charged to the working budget.
            bytes+=item.display.text.size()+item.display.title.size()+(setup.drawing ? 0 : original_bytes);
            if (bytes+item.display.pixels.size()>setup.content_memory) { r.problem="Combined notes exceed the content memory budget"; return; }
            if (setup.drawing) std::string().swap(item.display.pixels);
            remote.push_back(std::move(item));
        }
        // Every accepted file now lives in the complete remote snapshot.
        // Release duplicate listing metadata before creates/updates need TLS.
        std::vector<GoogleDriveFile>().swap(files);
        if (setup.drawing && bytes+largest_bitmap>setup.content_memory) { r.problem="Drawing metadata and active bitmap exceed the content working memory budget"; return; }
        auto load_drawing=[&](Remote &g) {
            if (!setup.drawing || !g.display.pixels.empty()) return true;
            size_t bitmap=g.display.width/8*g.display.height;
            if (bytes+bitmap>setup.content_memory) { r.problem="Drawing exceeds the content working memory budget"; return false; }
            GoogleNote current; PalmNote display;
            // The immutable revision from preflight prevents mixing PNGs from
            // different edits. Verify the projection before using this payload.
            if (!google.read(g.cloud.file,true,current)) { r.problem=google.error(); return false; }
            if (!project(current,setup,display,r.problem)) return false;
            if (note_fields(display,true)!=g.fields) { r.problem="Google drawing differs from the validated snapshot"; return false; }
            g.display.pixels=std::move(display.pixels); return true;
        };
        size_t old_size=state.map.size();
        state.map.erase(std::remove_if(state.map.begin(),state.map.end(),[&](const NoteSyncMap &m) {
            return m.pending!=1 && m.pending!=2 && std::none_of(local.begin(),local.end(),[&](const Local &l) { return l.id==m.palm_id; }) &&
                std::any_of(remote.begin(),remote.end(),[&](const Remote &g) { return g.cloud.file.id==m.google_id && g.missing; });
        }),state.map.end());
        if (state.map.size()!=old_size && !checkpoint()) return;
        for (const auto &m:state.map) if ((m.pending==1 || m.pending==2) && std::none_of(local.begin(),local.end(),[&](const Local &l) { return l.id==m.palm_id; })) {
            r.problem="A pending outgoing note is no longer on Palm. Restore its Palm backup before retrying."; return;
        }
        unsigned projected=local.size();
        for (const auto &g:remote) if (!g.missing && !g.moved && std::none_of(state.map.begin(),state.map.end(),[&](const NoteSyncMap &m) { return m.google_id==g.cloud.file.id; })) projected++;
        if (projected>setup.max_notes) { r.problem="Combined notes exceed the configured record limit"; return; }
        auto conflict=[&](const std::string &what) { r.problem="Both Palm and Google changed "+what+". Choose a conflict preference on the notes page or reconcile the edits first."; };
        for (auto &l:local) {
            auto m=std::find_if(state.map.begin(),state.map.end(),[&](const NoteSyncMap &v) { return v.palm_id==l.id; });
            if (l.attr&(HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED)) { r.skipped++; continue; }
            auto g=m==state.map.end() ? remote.end() : std::find_if(remote.begin(),remote.end(),[&](const Remote &v) { return v.cloud.file.id==m->google_id; });
            if (g!=remote.end() && g->moved) { r.skipped++; continue; }
            BitmapRelease remote_bitmap(setup.drawing && g!=remote.end() ? &g->display : nullptr);
            auto lf=l.fields;
            if (l.attr&HOTSYNC_ATTR_DELETED) {
                if (m!=state.map.end()) {
                    if (m->pending==2 && m->google_id.empty()) { r.problem="Resolve the pending Google creation before deleting this memo"; return; }
                    if (g!=remote.end() && !g->missing) {
                        if (g->fields!=m->fields) {
                            if (!setup.conflicts) { conflict("a note being deleted on Palm"); return; }
                            if (setup.conflicts==2) {
                                if (!load_drawing(*g)) return;
                                Local restored=l; restored.attr&=~HOTSYNC_ATTR_DELETED;
                                if (!write_palm(host,db,restored,g->display,setup.drawing,now,r.problem)) { if (r.problem.empty()) r.problem="Could not restore the edited Google note on Palm"; return; }
                                m->fields=g->fields; m->pending=0;
                                if (!checkpoint()) return;
                                r.downloaded++; continue;
                            }
                        }
                        if (!google.trash(g->cloud.file)) { r.problem=google.error(); return; }
                        g->missing=true;
                    }
                    state.map.erase(m); if (!checkpoint()) return;
                }
                if (!hotsync_delete_record(db,l.id)) { r.problem="Could not remove the Palm note tombstone"; return; }
                r.deleted++; continue;
            }
            if (m!=state.map.end() && !m->pending && g!=remote.end() && g->missing) {
                if (lf!=m->fields && !setup.conflicts) { conflict("a note deleted on Google"); return; }
                if (lf!=m->fields && setup.conflicts==1) {
                    state.map.erase(m); if (!checkpoint()) return; m=state.map.end();
                } else {
                    if (!hotsync_delete_record(db,l.id)) { r.problem="Could not delete the Palm note"; return; }
                    state.map.erase(m); if (!checkpoint()) return; r.deleted++; continue;
                }
            }
            bool creating=m==state.map.end() || m->pending==1 || m->pending==2;
            if (setup.drawing && !creating && !m->pending && g!=remote.end() && !g->missing &&
                !(l.attr&HOTSYNC_ATTR_DIRTY) && lf==m->fields && g->fields==m->fields) continue;
            if (m==state.map.end()) {
                if (state.map.size()>=setup.max_notes) { r.problem="Notes sync map is full"; return; }
                NoteSyncMap fresh; fresh.palm_id=l.id; fresh.fields=lf; fresh.pending=1;
                char marker[80]; snprintf(marker,sizeof(marker),"%c-%08lx-%06lx-%08lx-%08lx",setup.drawing ? 'n' : 'm',(unsigned long)user,(unsigned long)l.id,(unsigned long)host.random(),(unsigned long)host.random()); fresh.marker=marker;
                if (setup.drawing && !google.drive.generate_id(fresh.google_id)) { r.problem=google.drive.error(); return; }
                state.map.push_back(fresh); m=state.map.end()-1; if (!checkpoint()) return;
            }
            if (m->pending==3) {
                if (lf!=m->fields) { r.problem="An interrupted Palm import was edited; reconcile it before continuing"; return; }
                m->pending=0; if (!checkpoint()) return;
            }
            GoogleNote fresh_cloud;
            const GoogleNote *cloud=nullptr;
            if (g!=remote.end() && !g->missing) cloud=&g->cloud;
            else if (creating) {
                cloud=&fresh_cloud;
                if (!setup.drawing) {
                    if (m->google_id.empty()) {
                        m->pending=2; if (!checkpoint()) return;
                        std::string first=l.note.text.substr(0,l.note.text.find('\n'));
                        std::string title=palm_to_utf8(first.substr(0,80),setup.charset); if (title.empty()) title="Untitled memo";
                        GoogleDriveFile created;
                        if (!google.create_memo(state.folder,title,m->marker,created)) { r.problem=google.error(); return; }
                        m->google_id=created.id; m->pending=1; if (!checkpoint()) return;
                        fresh_cloud.file=created;
                    } else {
                        bool found; if (!google.drive.get(m->google_id,fresh_cloud.file,found)) { r.problem=google.drive.error(); return; }
                        if (!found) { r.problem="Pending Google memo disappeared; restore it from Trash before continuing"; return; }
                    }
                    if (!google.read(fresh_cloud.file,false,fresh_cloud)) { r.problem=google.error(); return; }
                } else { fresh_cloud.file.id=m->google_id; }
            } else { r.problem="Linked Google note is missing from the complete snapshot"; return; }
            if (setup.drawing) {
                size_t bitmap=l.note.width/8*l.note.height;
                if (bytes+bitmap>setup.content_memory) { r.problem="Drawing exceeds the content working memory budget"; return; }
                PalmNote current;
                if (!reload_drawing(host,db,l,current,r.problem)) return;
                l.note=std::move(current);
            }
            PalmNote target=std::move(l.note); auto rf=creating ? m->fields : g->fields;
            bool changed[2]={false,false};
            if (creating) {
                if (g!=remote.end() && (!g->display.text.empty() || setup.drawing)) {
                    auto pending_fields=g->fields;
                    if (pending_fields!=m->fields && pending_fields!=lf) { conflict("an unconfirmed new note"); return; }
                }
                changed[0]=true; changed[1]=setup.drawing;
            } else {
                for (unsigned f=0;f<(setup.drawing ? 2u : 1u);f++) {
                    bool palm=lf[f]!=m->fields[f],google_changed=rf[f]!=m->fields[f];
                    if (palm && google_changed && lf[f]!=rf[f] && !setup.conflicts) { conflict(setup.drawing ? (f ? "a drawing" : "a drawing title") : "a memo"); return; }
                    bool take_palm=palm && !(google_changed && setup.conflicts==2);
                    changed[f]=take_palm && lf[f]!=rf[f];
                    if (!take_palm && lf[f]!=rf[f]) {
                        if (!setup.drawing) target.text=g->display.text;
                        else if (!f) target.title=g->display.title;
                        else {
                            // The Palm fingerprint/header is already checked.
                            // Drop its pixels before decoding the chosen cloud
                            // image, so two drawings never overlap in RAM.
                            std::string().swap(target.pixels);
                            if (!load_drawing(*g)) return;
                            target.pixels=std::move(g->display.pixels); target.width=g->display.width; target.height=g->display.height;
                        }
                    }
                }
            }
            if (changed[0] || changed[1]) {
                if (!setup.drawing) {
                    if (!google.write_memo(*cloud,palm_to_utf8(target.text,setup.charset))) { r.problem=google.error(); return; }
                } else {
                    std::string title=palm_to_utf8(target.title,setup.charset); GoogleDriveFile saved;
                    bool fresh=cloud->file.revision.empty();
                    bool ok=!fresh && !changed[1] ? google.rename_drawing(cloud->file,title) :
                        google.write_drawing(cloud->file,fresh,state.folder,target,title,m->marker,host.now(),saved);
                    if (!ok) { r.problem=google.error(); return; }
                }
                r.uploaded++;
            }
            if (note_fields(target,setup.drawing)!=lf || (l.attr&HOTSYNC_ATTR_DIRTY)) {
                if (!write_palm(host,db,l,target,setup.drawing,now,r.problem)) { if (r.problem.empty()) r.problem="Could not update the Palm note"; return; }
                r.downloaded++;
            }
            m->fields=note_fields(target,setup.drawing); m->pending=0; if (!checkpoint()) return;
        }
        for (auto &g:remote) {
            if (g.missing || g.moved) continue;
            auto m=std::find_if(state.map.begin(),state.map.end(),[&](const NoteSyncMap &v) { return v.google_id==g.cloud.file.id; });
            if (m!=state.map.end() && std::any_of(local.begin(),local.end(),[&](const Local &l) { return l.id==m->palm_id; })) continue;
            BitmapRelease remote_bitmap(setup.drawing ? &g.display : nullptr);
            // A vanished record without a deletion tombstone is reimported.
            if (m!=state.map.end() && m->pending && m->pending!=3) { r.problem="A pending outgoing note is no longer on Palm"; return; }
            if (m==state.map.end()) {
                if (state.map.size()>=setup.max_notes) { r.problem="Notes sync map is full"; return; }
                NoteSyncMap imported; imported.google_id=g.cloud.file.id; imported.fields=g.fields; imported.pending=3;
                uint32_t id=(host.random()&0xffffffu); if (!id) id=1;
                for (unsigned tries=0;tries<=setup.max_notes*2;tries++,id=id==0xffffff ? 1 : id+1) {
                    if (std::none_of(local.begin(),local.end(),[&](const Local &l) { return l.id==id; }) && std::none_of(state.map.begin(),state.map.end(),[&](const NoteSyncMap &v) { return v.palm_id==id; })) { imported.palm_id=id; break; }
                }
                if (!imported.palm_id) { r.problem="Could not reserve a Palm note ID"; return; }
                state.map.push_back(imported); m=state.map.end()-1; if (!checkpoint()) return;
            }
            m->pending=3; m->fields=g.fields; if (!checkpoint()) return;
            if (!load_drawing(g)) return;
            Local fresh={m->palm_id,0,0,PalmNote(),{}};
            if (!write_palm(host,db,fresh,g.display,setup.drawing,now,r.problem)) { if (r.problem.empty()) r.problem="Could not import the Google note"; return; }
            m->pending=0; m->fields=g.fields; if (!checkpoint()) return; r.downloaded++;
        }
    };
    work(); r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK;
    if (opened && !r.link_lost && !hotsync_close_db(db) && r.problem.empty()) r.problem="Could not close the Palm notes database";
    r.link_lost=r.link_lost || hotsync_error()==HOTSYNC_ERR_LINK;
}
std::string notesync_summary(const NoteSyncResult &r) {
    std::string s=std::to_string(r.uploaded)+" uploaded, "+std::to_string(r.downloaded)+" downloaded, "+std::to_string(r.deleted)+" deleted";
    if (r.skipped) s+=", "+std::to_string(r.skipped)+" skipped";
    if (!r.problem.empty()) s+=". "+r.problem;
    return s;
}
