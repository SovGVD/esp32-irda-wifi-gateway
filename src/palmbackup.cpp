#include "palmbackup.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

static uint16_t u16(const uint8_t *p) { return (p[0]<<8)|p[1]; }
static uint32_t u32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|(p[2]<<8)|p[3]; }
static void p16(uint8_t *p,uint16_t n) { p[0]=n>>8; p[1]=n; }
static void p32(uint8_t *p,uint32_t n) { p[0]=n>>24; p[1]=n>>16; p[2]=n>>8; p[3]=n; }
static bool leap(unsigned y) { return y%4==0 && (y%100!=0 || y%400==0); }
static unsigned month_days(unsigned y,unsigned m) { static const unsigned d[]={31,28,31,30,31,30,31,31,30,31,30,31}; return d[m-1]+(m==2 && leap(y)); }
static uint32_t palm_time(const uint8_t *p) {
    unsigned y=u16(p),m=p[2],d=p[3];
    if (y<1904 || y>2040 || m<1 || m>12 || d<1 || d>month_days(y,m) || p[4]>23 || p[5]>59 || p[6]>59) return 0;
    uint64_t days=0;
    for (unsigned n=1904;n<y;n++) days+=365+leap(n);
    for (unsigned n=1;n<m;n++) days+=month_days(y,n);
    uint64_t seconds=((days+d-1)*24+p[4])*3600+p[5]*60+p[6];
    return seconds<=0xffffffffu ? (uint32_t)seconds : 0;
}
static void dlp_time(uint32_t n,uint8_t *p) {
    memset(p,0,8); if (!n) return;
    unsigned days=n/86400,y=1904,m=1;
    p[4]=(n%86400)/3600; p[5]=(n%3600)/60; p[6]=n%60;
    while (days>=365u+leap(y)) { days-=365u+leap(y); ++y; }
    while (days>=month_days(y,m)) { days-=month_days(y,m); m++; }
    p16(p,y); p[2]=m; p[3]=days+1;
}
int64_t palm_backup_time(const uint8_t date[8],int timezone_seconds) {
    auto value=palm_time(date); return value ? (int64_t)value-2082844800LL-timezone_seconds : 0;
}
void palm_backup_modified(HotSync_DBInfo &info,int64_t utc,int timezone_seconds) {
    int64_t value=utc+2082844800LL+timezone_seconds;
    if (value>0 && value<=0xffffffffu) dlp_time((uint32_t)value,info.modified);
}
static uint32_t hash_bytes(uint32_t h,const uint8_t *p,size_t n) { while (n--) { h^=*p++; h*=16777619u; } return h; }
static bool live(const PalmBackupImage::Entry &e,bool resource) { return resource || !(e.attr&(HOTSYNC_ATTR_DELETED|HOTSYNC_ATTR_ARCHIVED)); }

bool PalmBackupImage::entry(unsigned index,const PalmBackupRead &read,Entry &e,std::string &problem) const {
    if (index>=count) { problem="PDB entry index is out of range"; return false; }
    bool resource=info.flags&1; unsigned width=resource ? 10 : 8;
    uint8_t b[20]; size_t n=index+1<count ? width*2 : width;
    if (!read(78+index*width,b,n)) { problem="Could not read the PDB/PRC offset table"; return false; }
    e=Entry();
    e.offset=u32(b+(resource ? 6 : 0));
    uint32_t end=index+1<count ? u32(b+width+(resource ? 6 : 0)) : size;
    if (e.offset<data_offset || end<e.offset || end>size || end-e.offset>PALM_BACKUP_RECORD_MAX) {
        problem="Invalid PDB/PRC entry offsets or entry larger than classic HotSync supports"; return false;
    }
    e.size=end-e.offset;
    if (resource) { e.type=u32(b); e.resource_id=u16(b+4); }
    else { e.attr=b[4]&0xf0; e.category=b[4]&15; e.id=((uint32_t)b[5]<<16)|(b[6]<<8)|b[7];
        if (!e.id || (e.attr&HOTSYNC_ATTR_DELETED)) { problem="PDB contains an invalid/deleted record ID"; return false; }
    }
    return true;
}
bool PalmBackupImage::open(uint32_t bytes,const PalmBackupRead &read,std::string &problem) {
    *this=PalmBackupImage(); problem.clear(); uint8_t h[78];
    if (bytes<78 || !read(0,h,sizeof(h))) { problem="File has no complete PDB/PRC header"; return false; }
    const auto *end=(const uint8_t *)memchr(h,0,32);
    if (!end || end==h || u32(h+72)) { problem="Invalid database name or unsupported chained PDB record lists"; return false; }
    memcpy(info.name,h,end-h); info.flags=u16(h+32)&~0x8000u; info.version=u16(h+34);
    dlp_time(u32(h+36),info.created); dlp_time(u32(h+40),info.modified); dlp_time(u32(h+44),info.backed_up);
    info.modification=u32(h+48); app_offset=u32(h+52); sort_offset=u32(h+56);
    info.type=u32(h+60); info.creator=u32(h+64); count=u16(h+76); size=bytes;
    uint32_t table_end=78u+count*((info.flags&1) ? 10u : 8u);
    if (table_end>size || (info.flags&0x7080u)) { problem="Unsupported PDB database flags or truncated offset table"; return false; }
    data_offset=size;
    if (count) { uint8_t first[10]; if (!read(78,first,(info.flags&1) ? 10 : 8)) { problem="Could not read first PDB entry"; return false; }
        data_offset=u32(first+((info.flags&1) ? 6 : 0)); }
    if (data_offset<table_end || data_offset>size ||
        (app_offset && (app_offset<table_end || app_offset>data_offset)) ||
        (sort_offset && (sort_offset<(app_offset ? app_offset : table_end) || sort_offset>data_offset))) {
        problem="Invalid PDB/PRC application/sort block offsets"; return false;
    }
    if (app_offset) app_size=(sort_offset ? sort_offset : data_offset)-app_offset;
    if (sort_offset) sort_size=data_offset-sort_offset;
    if (app_size>PALM_BACKUP_RECORD_MAX || sort_size>PALM_BACKUP_RECORD_MAX) { problem="PDB application/sort block is too large"; return false; }
    record_buffer_size=std::max(app_size,sort_size);
    for (unsigned n=0;n<count;n++) {
        Entry e; if (!entry(n,read,e,problem)) return false;
        record_buffer_size=std::max(record_buffer_size,(size_t)e.size);
    }
    return true;
}
PalmBackupExport::~PalmBackupExport() { if (opened_) hotsync_close_db(handle_); }
bool PalmBackupExport::fail(const char *message) { problem_=std::string(message)+" (DLP "+std::to_string(hotsync_error())+")"; return false; }
bool PalmBackupExport::meta(unsigned index,PalmBackupImage::Entry &e,uint32_t &hash) {
    e=PalmBackupImage::Entry();
    if (info_.flags&1) {
        HotSync_Resource r; if (!hotsync_read_resource(handle_,index,0,0,&r)) return fail("Could not read resource metadata");
        e.type=r.type; e.resource_id=r.id; e.size=r.total_len;
    } else {
        HotSync_Record r; if (!hotsync_read_record_part(handle_,index,0,0,&r)) return fail("Could not read record metadata");
        e.id=r.id; e.attr=r.attr; e.category=r.category; e.size=r.total_len;
        if (!e.id || e.id>0xffffff || e.category>15) return fail("Invalid Palm record identity/category");
    }
    uint8_t b[16]={}; p32(b,e.id); p32(b+4,e.type); p32(b+8,e.size); p16(b+12,e.resource_id); b[14]=e.attr; b[15]=e.category;
    hash=hash_bytes(hash,b,sizeof(b)); return true;
}
bool PalmBackupExport::block_size(bool sort,uint32_t &size) {
    size=0;
    for (;;) {
        const uint8_t *p; size_t n;
        if (!hotsync_read_db_block(handle_,sort,(uint16_t)size,1024,&p,&n)) {
            if (hotsync_error()==HOTSYNC_DLP_NOT_FOUND && !size) return true;
            return fail("Could not read database app/sort information");
        }
        size+=n;
        if (size>PALM_BACKUP_RECORD_MAX) return fail("Database app/sort information exceeds classic HotSync limits");
        if (n<1024) return true;
    }
}
bool PalmBackupExport::prepare() {
    if (!hotsync_open_db(info_.name,HOTSYNC_MODE_READ|HOTSYNC_MODE_SECRET,&handle_)) return fail("Could not open database for backup");
    opened_=true;
    if (!hotsync_db_count(handle_,&raw_count_) || !block_size(false,app_size_) || !block_size(true,sort_size_)) return fail("Could not read database information");
    uint64_t bodies=0;
    record_buffer_size_=std::max(app_size_,sort_size_);
    for (unsigned n=0;n<raw_count_;n++) {
        PalmBackupImage::Entry e; if (!meta(n,e,hash_)) return false;
        if (!live(e,info_.flags&1)) continue;
        if (e.size>PALM_BACKUP_RECORD_MAX) return fail("Record/resource exceeds classic HotSync restore limit");
        count_++; bodies+=e.size;
        record_buffer_size_=std::max(record_buffer_size_,(size_t)e.size);
    }
    data_offset_=80u+count_*((info_.flags&1) ? 10u : 8u)+app_size_+sort_size_;
    if (bodies+data_offset_>0xffffffffu) return fail("Database is too large for a PDB file");
    size_=(uint32_t)bodies+data_offset_; table_offset_=data_offset_;
    piece_.assign(78,0); auto *h=(uint8_t *)&piece_[0];
    memcpy(h,info_.name,strlen(info_.name)); p16(h+32,info_.flags&~0x8000u); p16(h+34,info_.version);
    p32(h+36,palm_time(info_.created)); p32(h+40,palm_time(info_.modified)); p32(h+44,palm_time(info_.backed_up));
    p32(h+48,info_.modification); uint32_t start=80+count_*((info_.flags&1) ? 10 : 8);
    p32(h+52,app_size_ ? start : 0); p32(h+56,sort_size_ ? start+app_size_ : 0);
    p32(h+60,info_.type); p32(h+64,info_.creator); p32(h+68,1); p16(h+76,count_);
    stage_=1; return true;
}
std::string PalmBackupExport::signature() const {
    char b[100]; snprintf(b,sizeof(b),"%08lx-%08lx-%04x-%04x-%08lx-%08lx-%08lx-%08lx",(unsigned long)info_.modification,
        (unsigned long)hash_,info_.flags&~0x8000u,info_.version,(unsigned long)info_.type,(unsigned long)info_.creator,
        (unsigned long)app_size_,(unsigned long)sort_size_); return b;
}
bool PalmBackupExport::finish() {
    if (finished_) return true;
    while (index_<raw_count_) { PalmBackupImage::Entry e; if (!meta(index_++,e,body_hash_) || (live(e,info_.flags&1) && e.size)) return fail("Database changed during backup"); }
    HotSync_DBInfo now; uint16_t count;
    if (table_hash_!=hash_ || body_hash_!=hash_ || !hotsync_db_count(handle_,&count) || count!=raw_count_ ||
        !hotsync_read_db_info(info_.index,&now) || strcmp(now.name,info_.name) || now.modification!=info_.modification)
        return fail("Database changed during backup");
    finished_=true; return true;
}
bool PalmBackupExport::read(uint8_t *out,size_t want) {
    if (!opened_ || want>size_-sent_) return fail("Invalid backup stream request");
    size_t produced=0;
    while (produced<want) {
        if (piece_at_<piece_.size()) {
            size_t n=std::min(want-produced,piece_.size()-piece_at_); memcpy(out+produced,piece_.data()+piece_at_,n);
            produced+=n; piece_at_+=n; continue;
        }
        piece_.clear(); piece_at_=0;
        if (stage_==1) {
            if (index_<raw_count_) {
                PalmBackupImage::Entry e; if (!meta(index_++,e,table_hash_)) return false;
                if (!live(e,info_.flags&1)) continue;
                piece_.assign((info_.flags&1) ? 10 : 8,0); auto *p=(uint8_t *)&piece_[0];
                if (info_.flags&1) { p32(p,e.type); p16(p+4,e.resource_id); p32(p+6,table_offset_); }
                else { p32(p,table_offset_); p[4]=(e.attr&0xf0)|(e.category&15); p[5]=e.id>>16; p[6]=e.id>>8; p[7]=e.id; }
                table_offset_+=e.size; continue;
            }
            if (table_hash_!=hash_) return fail("Database changed while reading its offset table");
            stage_=2; piece_.assign(2,0); index_=0; offset_=0; continue;
        }
        if (stage_==2 || stage_==3) {
            uint32_t length=stage_==2 ? app_size_ : sort_size_;
            if (offset_==length) { stage_++; offset_=0; continue; }
            size_t n=std::min(want-produced,(size_t)(length-offset_)); const uint8_t *p; size_t got;
            if (!hotsync_read_db_block(handle_,stage_==3,offset_,n,&p,&got) || got!=n) return fail("Could not stream app/sort block");
            memcpy(out+produced,p,n); produced+=n; offset_+=n; continue;
        }
        if (!have_current_) {
            if (index_>=raw_count_) return fail("Backup stream ended prematurely");
            if (!meta(index_++,current_,body_hash_)) return false;
            if (!live(current_,info_.flags&1)) continue;
            have_current_=true; offset_=0;
        }
        if (offset_==current_.size) { have_current_=false; continue; }
        size_t n=std::min(want-produced,(size_t)(current_.size-offset_)); const uint8_t *p; size_t got;
        if (info_.flags&1) {
            HotSync_Resource r;
            if (!hotsync_read_resource(handle_,index_-1,offset_,n,&r) || r.type!=current_.type || r.id!=current_.resource_id || r.total_len!=current_.size) return fail("Resource changed during backup");
            p=r.data; got=r.len;
        } else {
            HotSync_Record r;
            if (!hotsync_read_record_part(handle_,index_-1,offset_,n,&r) || r.id!=current_.id || r.total_len!=current_.size) return fail("Record changed during backup");
            p=r.data; got=r.len;
        }
        if (got!=n) return fail("Palm returned an incomplete backup block");
        memcpy(out+produced,p,n); produced+=n; offset_+=n;
    }
    sent_+=produced;
    return sent_!=size_ || finish();
}
bool PalmBackupExport::rewind() {
    std::string expected=signature(); uint32_t bytes=size_; HotSync_DBInfo original=info_;
    if (!close()) return false;
    *this=PalmBackupExport(original);
    if (!prepare()) return false;
    if (size_!=bytes || signature()!=expected) return fail("Database changed before restarting the Drive upload");
    return true;
}
bool PalmBackupExport::close() {
    if (!opened_) return true;
    opened_=false; if (!hotsync_close_db(handle_)) return fail("Could not close backup database"); return true;
}
bool palm_backup_install(const PalmBackupImage &image,const PalmBackupRead &read,std::string &problem) {
    size_t room=0;
    if (!hotsync_record_buffer(&room) || room<image.record_buffer_size) {
        problem="Not enough HotSync buffer for database restore"; return false;
    }
    uint8_t db;
    if (!hotsync_create_db(image.info,&db)) { problem="Could not create Palm database (DLP "+std::to_string(hotsync_error())+")"; return false; }
    bool ok=true;
    auto copy=[&](uint32_t at,size_t n,const std::function<bool(const uint8_t *,size_t)> &write) {
        size_t room; uint8_t *p=hotsync_record_buffer(&room);
        if (n>room || !read(at,p,n) || !write(p,n)) { problem="Could not restore a database block (DLP "+std::to_string(hotsync_error())+")"; return false; }
        return true;
    };
    if (image.app_size) ok=copy(image.app_offset,image.app_size,[&](const uint8_t *p,size_t n) { return hotsync_write_db_block(db,false,p,n); });
    if (ok && image.sort_size) ok=copy(image.sort_offset,image.sort_size,[&](const uint8_t *p,size_t n) { return hotsync_write_db_block(db,true,p,n); });
    unsigned installed=0;
    for (unsigned i=0;ok && i<image.count;i++) {
        PalmBackupImage::Entry e;
        if (!image.entry(i,read,e,problem)) { ok=false; break; }
        if ((image.info.flags&1) && !e.size) continue;
        ok=copy(e.offset,e.size,[&](const uint8_t *p,size_t n) {
            if (image.info.flags&1) return hotsync_write_resource(db,e.type,e.resource_id,p,n);
            uint32_t id;
            return hotsync_write_record(db,e.id,e.attr,e.category,n,&id) && id==e.id;
        });
        if (ok) installed++;
    }
    uint16_t actual;
    if (ok && (!hotsync_db_count(db,&actual) || actual!=installed)) { problem="Restored database has duplicate or missing entry identities"; ok=false; }
    if (ok && !hotsync_set_db_info(db,image.info)) { problem="Could not restore database metadata"; ok=false; }
    int error=hotsync_error();
    if (error!=HOTSYNC_ERR_LINK && !hotsync_close_db(db)) { if (ok) problem="Could not close restored database"; ok=false; }
    // The caller journals this new database before creation, and handles
    // cleanup/retry. Never delete an existing database in this helper.
    return ok;
}
