#include "macho.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <functional>
namespace radek {
namespace {
struct Reader {
    const std::vector<uint8_t>& b; size_t base, size; bool be=false;
    void check(uint64_t p,uint64_t n)const{if(p>size||n>size-p)throw std::runtime_error("Mach-O range outside slice");}
    uint64_t u(size_t p,size_t n)const{check(p,n);uint64_t v=0;for(size_t i=0;i<n;i++)v|=uint64_t(b[base+p+i])<<(8*(be?n-i-1:i));return v;}
    std::string str(size_t p,size_t end)const{check(p,0);check(end,0);if(p>end)throw std::runtime_error("invalid string range");std::string s;while(p<end){auto c=b[base+p++];if(!c)return s;s+=char(c);}throw std::runtime_error("unterminated Mach-O string");}
    std::string fixed(size_t p,size_t n)const{check(p,n);std::string s;for(size_t i=0;i<n&&b[base+p+i];i++)s+=char(b[base+p+i]);return s;}
    uint64_t leb(size_t& p,size_t end)const{uint64_t n=0;for(unsigned shift=0;shift<64;shift+=7){if(p>=end)throw std::runtime_error("truncated ULEB128");uint8_t c=uint8_t(u(p++,1));if(shift==63&&(c&0x7e))throw std::runtime_error("ULEB128 overflow");n|=uint64_t(c&127)<<shift;if(!(c&128))return n;}throw std::runtime_error("ULEB128 overflow");}
};
Json object(){return Json::object();} Json array(){return Json::array();}
std::string arch(uint64_t c,uint64_t s){s&=0xffffff;if(c==0x100000c)return s==2?"arm64e":"arm64";if(c==12)return s==11?"armv7s":s==9?"armv7":"arm32-unknown";return "unsupported";}
Json relocations(Reader&r,size_t off,size_t n){
    if(n>1000000)throw std::runtime_error("too many relocations");
    r.check(off,n*8);Json a=array();
    for(size_t i=0;i<n;i++){uint64_t x=r.u(off+i*8,4),y=r.u(off+i*8+4,4);Json j=object();j["address"]=x;j["raw"]=y;
        bool scattered=(x&0x80000000)!=0;j["scattered"]=scattered;
        if(scattered){j["type"]=(x>>24)&15;j["pcRelative"]=bool(x&(1<<30));j["length"]=(x>>28)&3;j["value"]=y;}
        else{j["symbolIndex"]=y&0xffffff;j["pcRelative"]=bool(y&(1<<24));j["length"]=(y>>25)&3;j["external"]=bool(y&(1<<27));j["type"]=(y>>28)&15;}a.push(j);
    }return a;
}
Json thin(Reader r){
    auto magic=r.u(0,4);bool wide=magic==0xfeedfacf||magic==0xcffaedfe;
    if(magic!=0xfeedface&&magic!=0xfeedfacf&&magic!=0xcefaedfe&&magic!=0xcffaedfe)throw std::runtime_error("not a Mach-O slice");
    r.be=magic==0xcefaedfe||magic==0xcffaedfe;size_t hdr=wide?32:28;r.check(0,hdr);
    auto cpu=r.u(4,4),sub=r.u(8,4),nc=r.u(16,4),sz=r.u(20,4);
    if(nc>65536||sz<nc*8)throw std::runtime_error("invalid load command count");
    r.check(hdr,sz);
    Json j=object();j["architecture"]=arch(cpu,sub);j["cpuType"]=cpu;j["cpuSubtype"]=sub;j["fileType"]=r.u(12,4);j["flags"]=r.u(24,4);
    j["offset"]=uint64_t(r.base);j["size"]=uint64_t(r.size);j["bits"]=uint64_t(wide?64:32);j["bigEndian"]=r.be;j["encrypted"]=false;
    j["pacRequired"]=arch(cpu,sub)=="arm64e";j["segments"]=array();j["dependencies"]=array();j["symbols"]=array();j["imports"]=array();j["exports"]=array();j["loadCommands"]=array();j["metadata"]=array();j["linkedit"]=array();
    size_t symoff=0,nsyms=0,stroff=0,strsize=0;bool symSeen=false;size_t exportOff=0,exportSize=0;
    struct Bind{size_t off,size;std::string kind;};std::vector<Bind> binds;
    for(size_t p=hdr,k=0;k<nc;k++){
        auto cmd=r.u(p,4),len=r.u(p+4,4);if(len<8||len%4||len>hdr+sz-p)throw std::runtime_error("invalid load command size");size_t end=p+len;
        auto need=[&](size_t n){if(len<n)throw std::runtime_error("short load command");};
        Json lc=object();lc["command"]=cmd;lc["size"]=len;j["loadCommands"].push(lc);
        if(cmd==1||cmd==0x19){
            bool s64=cmd==0x19;size_t h=s64?72:56,ss=s64?80:68;need(h);size_t ns=r.u(p+(s64?64:48),4);if(ns>(len-h)/ss)throw std::runtime_error("section table exceeds segment command");
            Json s=object();s["name"]=r.fixed(p+8,16);s["vmAddress"]=r.u(p+24,s64?8:4);s["vmSize"]=r.u(p+(s64?32:28),s64?8:4);
            auto fo=r.u(p+(s64?40:32),s64?8:4),fs=r.u(p+(s64?48:36),s64?8:4);r.check(fo,fs);s["fileOffset"]=fo;s["fileSize"]=fs;s["maxProtection"]=r.u(p+(s64?56:40),4);s["initialProtection"]=r.u(p+(s64?60:44),4);s["sections"]=array();
            for(size_t z=0;z<ns;z++){
                size_t q=p+h+z*ss;Json sec=object();auto name=r.fixed(q,16);sec["name"]=name;sec["segment"]=r.fixed(q+16,16);sec["address"]=r.u(q+32,s64?8:4);auto size=r.u(q+(s64?40:36),s64?8:4),off=r.u(q+(s64?48:40),4),flags=r.u(q+(s64?64:56),4);auto type=flags&255;
                if(type!=1&&type!=12&&type!=18)r.check(off,size);
                sec["size"]=size;sec["offset"]=off;sec["alignment"]=r.u(q+(s64?52:44),4);sec["flags"]=flags;sec["relocations"]=relocations(r,r.u(q+(s64?56:48),4),r.u(q+(s64?60:52),4));s["sections"].push(sec);
                if(name.find("objc")!=std::string::npos||name.find("swift")!=std::string::npos||name=="__unwind_info"||name=="__eh_frame"||type==9||type==10||type==21){Json m=object();m["section"]=name;m["offset"]=off;m["size"]=size;m["status"]="metadata-only";j["metadata"].push(m);}
            }j["segments"].push(s);
        }else if(cmd==2){need(24);if(symSeen)throw std::runtime_error("duplicate symbol table");symSeen=true;symoff=r.u(p+8,4);nsyms=r.u(p+12,4);stroff=r.u(p+16,4);strsize=r.u(p+20,4);if(nsyms>1000000)throw std::runtime_error("symbol limit");r.check(symoff,nsyms*(wide?16:12));r.check(stroff,strsize);
        }else if(cmd==0xb){need(80);Json d=object();const char*names[]={"localIndex","localCount","externalIndex","externalCount","undefinedIndex","undefinedCount","tocOffset","tocCount","moduleOffset","moduleCount","referenceOffset","referenceCount","indirectOffset","indirectCount","externalRelocationOffset","externalRelocationCount","localRelocationOffset","localRelocationCount"};for(size_t x=0;x<18;x++)d[names[x]]=r.u(p+8+x*4,4);
            r.check(r.u(p+56,4),r.u(p+60,4)*4);d["externalRelocations"]=relocations(r,r.u(p+64,4),r.u(p+68,4));d["localRelocations"]=relocations(r,r.u(p+72,4),r.u(p+76,4));j["dynamicSymbols"]=d;
        }else if(cmd==0xc||cmd==0x18||cmd==0x80000018||cmd==0x8000001f||cmd==0x80000023||cmd==0x20||cmd==0xd){need(24);auto no=r.u(p+8,4);if(no<24||no>=len)throw std::runtime_error("invalid dylib name offset");Json d=object();d["path"]=r.str(p+no,end);d["command"]=cmd;d["currentVersion"]=r.u(p+16,4);d["compatibilityVersion"]=r.u(p+20,4);if(cmd==0xd)j["dylibIdentity"]=d;else j["dependencies"].push(d);
        }else if(cmd==0x8000001c){need(12);auto no=r.u(p+8,4);if(no<12||no>=len)throw std::runtime_error("invalid rpath");if(!j.fields.count("rpaths"))j["rpaths"]=array();j["rpaths"].push(r.str(p+no,end));
        }else if(cmd==0x21||cmd==0x2c){need(cmd==0x21?20:24);auto off=r.u(p+8,4),size=r.u(p+12,4);r.check(off,size);Json e=object();e["offset"]=off;e["size"]=size;e["cryptId"]=r.u(p+16,4);j["encryption"]=e;if(r.u(p+16,4))j["encrypted"]=true;
        }else if(cmd==0x80000028){need(24);auto off=r.u(p+8,8);r.check(off,1);j["entryOffset"]=off;j["stackSize"]=r.u(p+16,8);
        }else if(cmd==4||cmd==5){need(16);Json t=object();t["flavor"]=r.u(p+8,4);t["count"]=r.u(p+12,4);if(t.fields["count"].value.empty())throw std::runtime_error("invalid thread state");if(r.u(p+12,4)>(len-16)/4)throw std::runtime_error("thread state truncated");j["threadEntry"]=t;
        }else if(cmd==0x22||cmd==0x80000022){need(48);const char*names[]={"rebase","bind","weakBind","lazyBind","exportTrie"};for(size_t x=0;x<5;x++){size_t off=r.u(p+8+x*8,4),n=r.u(p+12+x*8,4);r.check(off,n);Json l=object();l["kind"]=names[x];l["offset"]=uint64_t(off);l["size"]=uint64_t(n);j["linkedit"].push(l);if(x==4){exportOff=off;exportSize=n;}else if(x>0&&n)binds.push_back({off,n,names[x]});}
        }else if(cmd==0x1d||cmd==0x1e||cmd==0x26||cmd==0x29||cmd==0x80000033||cmd==0x80000034){need(16);size_t off=r.u(p+8,4),n=r.u(p+12,4);r.check(off,n);Json l=object();l["command"]=cmd;l["offset"]=uint64_t(off);l["size"]=uint64_t(n);j["linkedit"].push(l);
            if(cmd==0x80000033){exportOff=off;exportSize=n;}
            if(cmd==0x80000034&&n){if(n<28)throw std::runtime_error("short chained fixups");Json f=object();f["version"]=r.u(off,4);f["startsOffset"]=r.u(off+4,4);auto io=r.u(off+8,4),so=r.u(off+12,4),count=r.u(off+16,4),format=r.u(off+20,4);f["importsFormat"]=format;f["symbolsFormat"]=r.u(off+24,4);f["importsCount"]=count;f["imports"]=array();
                if(io>n||so>n||r.u(off+4,4)>=n||count>1000000)throw std::runtime_error("invalid chained fixup offsets");
                size_t stride=format==1?4:format==2?8:format==3?16:0;
                if(!stride||count>(n-io)/stride)throw std::runtime_error("invalid chained import format/table");
                if(r.u(off+24,4)==0){for(size_t z=0;z<count;z++){uint64_t word=r.u(off+io+z*stride,format==3?8:4),no=format==3?word>>32:word>>9;if(no>=n-so)throw std::runtime_error("invalid chained symbol");Json im=object();im["name"]=r.str(off+so+no,off+n);im["ordinal"]=word&(format==3?65535:255);im["weak"]=bool(word&(format==3?65536:256));f["imports"].push(im);j["imports"].push(im);}}j["chainedFixups"]=f;
            }
            if(cmd==0x1d&&n){Reader cr=r;cr.be=true;if(n<12||cr.u(off,4)!=0xfade0cc0)throw std::runtime_error("invalid signature superblob");size_t total=cr.u(off+4,4),count=cr.u(off+8,4);if(total>n||total<12||count>(total-12)/8)throw std::runtime_error("invalid signature index");Json cs=object();cs["cryptographicVerification"]="not-performed";cs["blobs"]=array();for(size_t x=0;x<count;x++){auto bo=cr.u(off+16+x*8,4);if(bo>total||total-bo<8)throw std::runtime_error("invalid signature blob");auto bl=cr.u(off+bo+4,4);if(bl<8||bl>total-bo)throw std::runtime_error("invalid signature length");Json b=object();auto bm=cr.u(off+bo,4);b["magic"]=bm;b["length"]=bl;b["slot"]=cr.u(off+12+x*8,4);if(bm==0xfade0c02){if(bl<44)throw std::runtime_error("short code directory");b["version"]=cr.u(off+bo+8,4);b["flags"]=cr.u(off+bo+12,4);auto id=cr.u(off+bo+20,4);if(id>=bl)throw std::runtime_error("invalid signature identifier");b["identifier"]=cr.str(off+bo+id,off+bo+bl);}cs["blobs"].push(b);}j["codeSignature"]=cs;}
        }
        p=end;if(k+1==nc&&p!=hdr+sz)throw std::runtime_error("load command byte count mismatch");
    }
    for(size_t i=0;i<nsyms;i++){size_t p=symoff+i*(wide?16:12);auto index=r.u(p,4),type=r.u(p+4,1);if(index>=strsize)throw std::runtime_error("invalid symbol string offset");Json s=object();s["name"]=r.str(stroff+index,stroff+strsize);s["type"]=type;s["section"]=r.u(p+5,1);s["description"]=r.u(p+6,2);s["value"]=r.u(p+8,wide?8:4);j["symbols"].push(s);if(!(type&0xe0)&&(type&1)){if((type&0xe)==0)j["imports"].push(s);else if((type&0xe)==0xe)j["exports"].push(s);}}
    for(auto&b:binds){size_t p=b.off,end=p+b.size;std::string symbol;uint64_t ordinal=0,seg=0,address=0;while(p<end){auto byte=r.u(p++,1),op=byte&0xf0,imm=byte&15;switch(op){case 0: symbol.clear();break;case 0x10:ordinal=imm;break;case 0x20:ordinal=r.leb(p,end);break;case 0x30:ordinal=imm?uint64_t(int64_t(int8_t(imm|0xf0))):0;break;case 0x40:symbol=r.str(p,end);p+=symbol.size()+1;break;case 0x50:break;case 0x60:r.leb(p,end);break;case 0x70:seg=imm;address=r.leb(p,end);break;case 0x80:address+=r.leb(p,end);break;
                case 0x90:case 0xa0:case 0xb0:case 0xc0:{if(symbol.empty())throw std::runtime_error("bind without symbol");Json im=object();im["name"]=symbol;im["ordinal"]=ordinal;im["segment"]=seg;im["offset"]=address;im["stream"]=b.kind;j["imports"].push(im);if(op==0xa0)address+=r.leb(p,end);if(op==0xc0){r.leb(p,end);r.leb(p,end);}address+=wide?8:4;break;}
                case 0xd0:throw std::runtime_error("threaded dyld bind opcode unsupported");default:throw std::runtime_error("invalid bind opcode");}}}
    if(exportSize){std::set<size_t> active;size_t visited=0;std::function<void(size_t,std::string)> walk=[&](size_t node,std::string prefix){if(++visited>100000||prefix.size()>4096||active.size()>256||node>=exportSize||!active.insert(node).second)throw std::runtime_error("cyclic/oversized export trie");size_t p=exportOff+node,end=exportOff+exportSize;size_t len=r.leb(p,end);if(len>end-p)throw std::runtime_error("truncated export terminal");size_t next=p+len;if(len){Json e=object();e["name"]=prefix;auto flags=r.leb(p,next);e["flags"]=flags;if(flags&8){e["ordinal"]=r.leb(p,next);e["importName"]=r.str(p,next);}else{e["address"]=r.leb(p,next);if(flags&16)e["resolver"]=r.leb(p,next);}j["exports"].push(e);}p=next;if(p>=end)throw std::runtime_error("truncated export children");size_t count=r.u(p++,1);for(size_t x=0;x<count;x++){auto edge=r.str(p,end);p+=edge.size()+1;auto child=r.leb(p,end);walk(child,prefix+edge);}active.erase(node);};walk(0,"");}
    return j;
}
}
Json analyze(const std::vector<uint8_t>& data){
    Reader r{data,0,data.size(),true};auto m=r.u(0,4);Json result=object();result["schemaVersion"]=uint64_t(1);result["slices"]=array();
    if(m==0xcafebabe||m==0xcafebabf||m==0xbebafeca||m==0xbfbafeca){bool wide=m==0xcafebabf||m==0xbfbafeca;r.be=m==0xcafebabe||m==0xcafebabf;auto n=r.u(4,4);size_t stride=wide?32:20;if(n==0||n>64)throw std::runtime_error("invalid FAT slice count");r.check(8,n*stride);std::vector<std::pair<uint64_t,uint64_t>> ranges;
        for(size_t i=0;i<n;i++){size_t p=8+i*stride;auto off=r.u(p+8,wide?8:4),size=r.u(p+(wide?16:12),wide?8:4),align=r.u(p+(wide?24:16),4);r.check(off,size);if(off<8+n*stride||!size||align>31||off%(uint64_t(1)<<align))throw std::runtime_error("invalid FAT alignment/range");for(auto [a,b]:ranges)if(off<b&&a<off+size)throw std::runtime_error("overlapping FAT slices");ranges.emplace_back(off,off+size);auto s=thin(Reader{data,size_t(off),size_t(size)});if(s.fields["cpuType"].value!=std::to_string(r.u(p,4))||s.fields["cpuSubtype"].value!=std::to_string(r.u(p+4,4)))throw std::runtime_error("FAT architecture mismatch");result["slices"].push(s);}
    }else result["slices"].push(thin(Reader{data,0,data.size()}));return result;
}
}
