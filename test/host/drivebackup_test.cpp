#include "drivebackup.h"
#include "calendartext.h"
#include "cJSON.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

static int fd=-1;
static int port_read(uint8_t *p,size_t max,uint32_t ms) { pollfd f={fd,POLLIN,0}; if (poll(&f,1,ms)<=0) return 0; int n=::read(fd,p,max); return n>0 ? n : -1; }
static bool port_write(const uint8_t *p,size_t n) { return ::write(fd,p,n)==(ssize_t)n; }
static uint32_t millis_now() { timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000+t.tv_nsec/1000000; }
#include "drive_server.h"
class Host : public DriveBackupHost {
public:
    Server server; DriveBackupState state; bool writable=true,connected=true; int64_t clock=when("2026-10-15T07:00:00Z");
    std::function<bool(size_t)> reserve;
    bool deny_reserve=false;
    size_t reserve_calls=0,last_reserve=0;
    bool reserve_record_buffer(size_t bytes) override {
        reserve_calls++; last_reserve=bytes;
        return !deny_reserve && (reserve ? reserve(bytes) : DriveBackupHost::reserve_record_buffer(bytes));
    }
    GoogleHttp &http() override { return server; }
    bool online() override { return connected; }
    int64_t now() override { return clock; }
    bool access_token(std::string &token,std::string &) override { token="token"; return true; }
    bool load(DriveBackupState &out) override { out=state; return true; }
    bool store(const DriveBackupState &in) override { if (!writable) return false; state=in; return true; }
};
static DriveBackupResult sync(Host &h,const DriveBackupSetup &setup,bool ok=true) {
    DriveBackupResult r; drivebackup_run(setup,h,r); printf("Drive: %s\n",drivebackup_summary(r).c_str()); fflush(stdout);
    assert(r.problem.empty()==ok && !r.link_lost); return r;
}
static File &named(Host &h,const char *name) { for (auto &f:h.server.files) if (f.second.name==name) return f.second; assert(false); return h.server.files.begin()->second; }
static std::string export_db(const char *name) {
    for (unsigned index=0;index<100;index++) {
        HotSync_DBInfo i; assert(hotsync_read_db_info(index,&i));
        if (strcmp(i.name,name)) continue;
        PalmBackupExport stream(i); assert(stream.prepare()); std::string data(stream.size(),0);
        size_t at=0; while (at<data.size()) { size_t n=std::min((size_t)173,data.size()-at); assert(stream.read((uint8_t *)&data[at],n)); at+=n; }
        assert(stream.close()); return data;
    }
    assert(false); return "";
}
static PalmBackupImage image(const std::string &data) {
    PalmBackupImage i; std::string problem;
    assert(i.open(data.size(),[&](uint32_t at,uint8_t *p,size_t n) { if (at>data.size() || n>data.size()-at) return false; memcpy(p,data.data()+at,n); return true; },problem)); return i;
}
static std::string first_record(const char *name) {
    uint8_t db; assert(hotsync_open_db(name,HOTSYNC_MODE_READ|HOTSYNC_MODE_SECRET,&db)); HotSync_Record r;
    assert(hotsync_read_record_by_index(db,0,&r)); std::string text((const char *)r.data,r.len); assert(hotsync_close_db(db)); return text;
}
static void remove_db(const char *name) { assert(hotsync_delete_db(name)); }
int main(int argc,char **argv) {
    assert(argc==2); fd=open(argv[1],O_RDWR|O_NOCTTY); assert(fd>=0);
    size_t memory_size=HOTSYNC_BUFFER_SIZE(8000); auto *memory=(uint8_t *)malloc(memory_size); assert(memory);
    HotSync_Io io={port_read,port_write,millis_now}; assert(hotsync_accept(&io,memory,memory_size,10000));
    uint32_t rom; assert(hotsync_read_sys_info(&rom));
    Host h; DriveBackupSetup setup;
    h.reserve=[&](size_t bytes) {
        size_t room=0; hotsync_record_buffer(&room); if (bytes<=room) return true;
        auto *next=(uint8_t *)realloc(memory,HOTSYNC_BUFFER_SIZE(bytes));
        if (!next) return false;
        memory=next; memory_size=HOTSYNC_BUFFER_SIZE(bytes); return hotsync_resize_buffer(memory,memory_size);
    };
    h.server.lose_folder=true; h.server.lose_upload=true;
    auto r=sync(h,setup,false); assert(h.state.pending=='u' && h.server.files.size()==5);
    r=sync(h,setup); assert(r.uploaded==2 && !h.state.pending && h.server.files.size()==6 && h.server.chunks>=4);
    assert(named(h,"Backup").parent=="my-root" && named(h,"Palm").parent==named(h,"Backup").id && named(h,"Test Palm").parent==named(h,"Palm").id);
    std::string memo_id=named(h,"MemoDB.pdb").id,app_id=named(h,"Example.prc").id;
    auto memo=image(h.server.files[memo_id].data),app=image(h.server.files[app_id].data);
    assert(memo.count==2 && memo.app_size==18 && memo.sort_size==5 && app.count==5 && (app.info.flags&1));
    assert(h.server.files[memo_id].data==export_db("MemoDB") && h.server.files[app_id].data==export_db("Example"));
    unsigned uploads=h.server.uploads;
    r=sync(h,setup); assert(r.unchanged==2 && h.server.uploads==uploads);
    // A fresh-session 404 restarts once using the same file ID. Expiry after
    // a 256 KiB chunk must rewind the entire PRC, not just the last chunk.
    h.server.files[memo_id].props["palmSignature"]="force 404 retry";
    auto sessions=h.server.session_starts; h.server.expired_sessions=1;
    r=sync(h,setup); assert(r.uploaded==1 && h.server.session_starts==sessions+2 && h.server.files.size()==6 && h.server.files[memo_id].data==export_db("MemoDB"));
    h.server.files[app_id].props["palmSignature"]="force later expiry";
    sessions=h.server.session_starts; h.server.expired_sessions=1; h.server.expire_after_chunk=true;
    r=sync(h,setup); assert(r.uploaded==1 && h.server.session_starts==sessions+2 && h.server.files[app_id].data==export_db("Example"));
    h.server.expire_after_chunk=false; h.server.expired_sessions=2;
    h.server.files[memo_id].props["palmSignature"]="persistent 404"; sessions=h.server.session_starts;
    r=sync(h,setup,false); assert(r.problem.find("HTTP 404")!=std::string::npos && h.server.session_starts==sessions+2 && h.state.pending=='u');
    sync(h,setup); assert(!h.state.pending && h.server.files.size()==6);
    // New Drive PDB/PRC files install automatically, preserving IDs, flags,
    // app/sort blocks and data. Restore happens before backup in firmware.
    assert(h.reserve_calls==0 && memory_size==HOTSYNC_BUFFER_SIZE(8000));
    std::string new_data=pdb("NewNotes","From Drive\0");
    auto &added=h.server.add("NewNotes.pdb",h.state.user_folder,new_data,h.clock+30); std::string new_id=added.id;
    DriveBackupSetup restore=setup; restore.restore_only=true;
    h.deny_reserve=true;
    auto denied=sync(h,restore,false); assert(denied.problem.find("memory")!=std::string::npos && !h.state.pending);
    uint8_t missing; assert(!hotsync_open_db("NewNotes",HOTSYNC_MODE_READ,&missing) && hotsync_error()==HOTSYNC_DLP_NOT_FOUND);
    h.deny_reserve=false;
    r=sync(h,restore); assert(r.restored==1 && r.uploaded==0 && first_record("NewNotes")=="From Drive");
    // Backup-only export also handles large resources with the normal small
    // Mail buffer, avoiding a second 64 KiB allocation on the ESP32.
    memory_size=HOTSYNC_BUFFER_SIZE(8000); memory=(uint8_t *)realloc(memory,memory_size); assert(memory && hotsync_resize_buffer(memory,memory_size));
    h.server.files[app_id].props["palmSignature"]="force reupload";
    DriveBackupSetup backup=setup; backup.backup_only=true; r=sync(h,backup); assert(r.uploaded==2 && r.unchanged==1);
    assert(h.server.files[app_id].data==export_db("Example"));
    assert(h.server.files[new_id].props["palmBackup"]=="1");
    // Newer Drive data replaces an existing Palm DB only after saving its old
    // version to Recovery. A subsequent sync must not keep restoring it.
    auto &edited=h.server.files[new_id]; edited.data=pdb("NewNotes","Newest Drive data"); edited.modified=h.clock+60; h.server.commit(edited);
    h.deny_reserve=true; sync(h,setup,false);
    assert(!h.state.pending && first_record("NewNotes")=="From Drive" && h.state.recovery_folder.empty());
    h.deny_reserve=false;
    r=sync(h,setup); assert(r.restored==1 && first_record("NewNotes")=="Newest Drive data" && !h.state.recovery_folder.empty());
    bool recovery=false; for (const auto &f:h.server.files) if (f.second.parent==h.state.recovery_folder) { recovery=true; assert(f.second.data.find("From Drive")!=std::string::npos); }
    assert(recovery); r=sync(h,setup); assert(!r.restored && r.unchanged==3);
    // Palm wins when Drive is older or timestamps tie, even if the external
    // edit left our private appProperties in place.
    auto palm_copy=export_db("NewNotes"); edited=h.server.files[new_id];
    h.server.files[new_id].data=pdb("NewNotes","Older file"); h.server.files[new_id].modified=h.clock-3600; h.server.commit(h.server.files[new_id]);
    r=sync(h,setup); assert(!r.restored && r.uploaded==1 && first_record("NewNotes")=="Newest Drive data" && h.server.files[new_id].data==palm_copy);
    h.server.files[new_id].data=pdb("NewNotes","Tie file"); h.server.commit(h.server.files[new_id]);
    r=sync(h,setup); assert(!r.restored && r.uploaded==1 && first_record("NewNotes")=="Newest Drive data");
    // Removing a Palm DB retains the cloud backup; one-time restore recovers
    // it rather than silently reinstalling every intentionally removed app.
    // Even a tiny replacement needs capacity for the larger original in
    // case rollback is necessary. Admission failure must leave it intact.
    auto original_app=h.server.files[app_id];
    h.server.files[app_id].data=pdb("Example","tiny"); h.server.files[app_id].modified=h.clock+60;
    h.server.commit(h.server.files[app_id]); h.deny_reserve=true;
    sync(h,setup,false); assert(h.last_reserve==60000 && !h.state.pending && export_db("Example")==original_app.data);
    h.server.files[app_id]=original_app; h.deny_reserve=false;
    remove_db("Example"); r=sync(h,setup); assert(r.retained==1 && h.server.files.count(app_id));
    // A reset between deletion and creation also needs the original's
    // larger rollback buffer, although no partial Palm DB exists yet.
    auto before_interruption=h.state;
    auto recovery_id=h.server.add("Previous Example.prc","Recovery",original_app.data,h.clock).id;
    h.server.files[app_id].data=pdb("Example","tiny"); h.server.files[app_id].modified=h.clock+60;
    h.server.commit(h.server.files[app_id]);
    h.state.pending='r'; h.state.pending_file=app_id; h.state.pending_db="Example"; h.state.recovery_file=recovery_id;
    h.deny_reserve=true; sync(h,setup,false); assert(h.last_reserve==60000 && h.state.pending=='r');
    assert(!hotsync_open_db("Example",HOTSYNC_MODE_READ,&missing) && hotsync_error()==HOTSYNC_DLP_NOT_FOUND);
    h.deny_reserve=false; h.state=before_interruption; h.server.files[app_id]=original_app; h.server.files.erase(recovery_id);
    setup.restore_saved=true; r=sync(h,setup); assert(h.last_reserve==60000 && memory_size==HOTSYNC_BUFFER_SIZE(60000)); assert(r.restored==1 && export_db("Example")==h.server.files[app_id].data); setup.restore_saved=false;
    // A failed recovery upload leaves the original Palm untouched; retry
    // completes the persisted save-before-replace operation first.
    auto old_notes=first_record("NewNotes");
    h.server.files[new_id].data=pdb("NewNotes",std::string(9000,'n')); h.server.files[new_id].modified=h.clock+120; h.server.commit(h.server.files[new_id]);
    h.server.fail_recovery=true; sync(h,setup,false); assert(h.state.pending=='s' && first_record("NewNotes")==old_notes);
    // Failure partway through installation rolls back from Recovery, rather
    // than allowing a partial Palm database into the next backup.
    h.server.fail_recovery=false; h.server.fail_range_id=new_id;
    sync(h,setup,false); assert(!h.state.pending && first_record("NewNotes")==old_notes);
    h.server.fail_range_id.clear(); r=sync(h,setup); assert(r.restored==1 && first_record("NewNotes")==std::string(9000,'n'));
    // A newer Drive edit between the restore and backup phases is retained.
    h.server.files[new_id].data=pdb("NewNotes","Between phases"); h.server.files[new_id].modified=h.clock+180; h.server.commit(h.server.files[new_id]);
    sync(h,backup,false); assert(first_record("NewNotes")==std::string(9000,'n') && h.server.files[new_id].data.find("Between phases")!=std::string::npos);
    r=sync(h,setup); assert(r.restored==1 && first_record("NewNotes")=="Between phases");
    // A Drive edit after opening a resumable update aborts the old upload.
    h.server.files[new_id].data=pdb("NewNotes","Old version"); h.server.files[new_id].modified=h.clock-3600; h.server.commit(h.server.files[new_id]);
    h.server.race_upload=true; sync(h,setup,false); assert(h.server.files[new_id].data.find("Concurrent Drive edit")!=std::string::npos);
    h.server.files[new_id].modified=h.clock+240; r=sync(h,setup); assert(r.restored==1 && first_record("NewNotes")=="Concurrent Drive edit");
    // Duplicate database names, malformed tables and partial downloads never
    // install a partial snapshot or overwrite valid Palm/cloud data.
    unsigned before=h.server.uploads;
    auto duplicate=h.server.add("Another.pdb",h.state.user_folder,pdb("NewNotes","duplicate"),h.clock+120).id;
    sync(h,setup,false); assert(h.server.uploads==before); h.server.files.erase(duplicate);
    auto bad=h.server.add("Broken.pdb",h.state.user_folder,pdb("Broken","bad"),h.clock+120).id;
    p32(h.server.files[bad].data,78,3); h.server.commit(h.server.files[bad]); sync(h,setup,false); assert(h.server.uploads==before); h.server.files.erase(bad);
    auto incoming=h.server.add("Range.pdb",h.state.user_folder,pdb("Range",std::string(9000,'r')),h.clock+120).id;
    h.server.short_range=true; sync(h,setup,false); h.server.short_range=false;
    uint8_t db; assert(!hotsync_open_db("Range",HOTSYNC_MODE_READ,&db) && hotsync_error()==HOTSYNC_DLP_NOT_FOUND);
    // An interrupted install cannot be backed up as a complete database. Retry
    // is explicit and validates the source before deleting the partial DB.
    HotSync_DBInfo partial={}; strcpy(partial.name,"Range"); partial.type=0x44415441; partial.creator=0x54737432;
    assert(hotsync_create_db(partial,&db) && hotsync_close_db(db));
    h.state.pending='r'; h.state.pending_file=incoming; h.state.pending_db="Range";
    sync(h,setup,false); setup.retry_interrupted=true;
    h.deny_reserve=true; sync(h,setup,false);
    assert(h.state.pending=='r' && hotsync_open_db("Range",HOTSYNC_MODE_READ,&db)); assert(hotsync_close_db(db));
    h.deny_reserve=false; r=sync(h,setup); assert(r.restored==1 && first_record("Range")==std::string(9000,'r')); setup.retry_interrupted=false;
    auto state=h.state; h.state.account="another-account"; sync(h,setup,false); h.state=state;
    h.connected=false; sync(h,setup,false); h.connected=true;
    h.server.deny=true; sync(h,setup,false); h.server.deny=false;
    Host no_space; no_space.writable=false; sync(no_space,setup,false); assert(no_space.server.files.size()==1);
    Host wrong_clock; wrong_clock.clock-=86400*4; sync(wrong_clock,setup,false); assert(wrong_clock.server.files.size()==1);
    // A reset Palm only regains the saved HotSync identity after an explicit
    // full-restore request with the matching folder user name/account.
    assert(hotsync_write_user(0,"")); DriveBackupSetup reset=setup; reset.username="Test Palm";
    sync(h,reset,false); reset.restore_saved=true; r=sync(h,reset);
    uint32_t restored_id; char restored_name[64]; assert(hotsync_read_user(&restored_id,restored_name,sizeof(restored_name)) && restored_id==1 && !strcmp(restored_name,"Test Palm"));
    assert(hotsync_add_log("Drive backup tests passed\n") && hotsync_end(0));
    free(memory); close(fd); puts("done");
}
