#include "nexodion/game_metadata.hpp"
#include <fstream>
#include <map>
#include <iterator>
namespace nexodion {

// Bounded SFO table parser over raw bytes. Never trusts offsets beyond the
// explicit length/count bounds; the file variant only wraps this plus the
// sibling icon0.png lookup.
ParamMetadata parse_param_sfo_bytes(const std::vector<unsigned char>& bytes) {
    ParamMetadata meta;
    if (bytes.size()<20 || bytes[0]!=0 || bytes[1]!='P' || bytes[2]!='S' || bytes[3]!='F') return meta;
    auto u16=[&](size_t p){return static_cast<uint32_t>(bytes[p])|(static_cast<uint32_t>(bytes[p+1])<<8);};
    auto u32=[&](size_t p){return u16(p)|(u16(p+2)<<16);};
    const size_t keys=u32(8),data=u32(12),count=u32(16);
    if(count>4096 || count>(bytes.size()-20)/16 || keys<20+count*16 || data<keys || data>bytes.size()) return meta;
    std::map<std::string,std::string> values;
    for(size_t i=0;i<count;++i) {
        const size_t p=20+i*16,key_offset=u16(p),length=u32(p+4),capacity=u32(p+8),offset=u32(p+12);
        if(key_offset>=data-keys || length>capacity || offset>bytes.size()-data || capacity>bytes.size()-data-offset) return meta;
        size_t end=keys+key_offset;while(end<data && bytes[end]!=0)++end;
        if(end==data) return meta;
        const std::string key(reinterpret_cast<const char*>(bytes.data()+keys+key_offset),end-keys-key_offset);
        if(u16(p+2)!=0x0204) continue;
        if(length==0 || bytes[data+offset+length-1]!=0) return meta;
        if(!values.emplace(key,std::string(reinterpret_cast<const char*>(bytes.data()+data+offset),length-1)).second) return meta;
    }
    meta.title_id=values["TITLE_ID"];meta.title_name=values["TITLE"];meta.category=values["CATEGORY"];
    meta.version=values["APP_VER"];meta.kind=parse_item_kind(meta.category);
    meta.valid=is_valid_title_id(meta.title_id);meta.confidence=!meta.valid?MetadataConfidence::Unknown:
        (meta.title_id.starts_with("PPSA") || meta.title_id.starts_with("CUSA")?MetadataConfidence::Verified:MetadataConfidence::Manifest);
    return meta;
}

ParamMetadata parse_param_sfo_file(const std::filesystem::path& path) {
    ParamMetadata meta;
    std::error_code ec;const auto size=std::filesystem::file_size(path,ec);
    if(ec || size<20 || size>1024*1024) return meta;
    std::ifstream file(path,std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),{});
    if(bytes.size()!=size) return meta;
    meta=parse_param_sfo_bytes(bytes);
    if(!meta.valid) return meta;
    const auto icon=path.parent_path()/"icon0.png";if(std::filesystem::exists(icon,ec))meta.icon_path=icon.string();
    return meta;
}
}
