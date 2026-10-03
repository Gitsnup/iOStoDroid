#pragma once
#include <map>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstdint>
namespace radek {
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind=Null;
    std::string value;
    std::vector<Json> items;
    std::map<std::string,Json> fields;
    Json()=default;
    Json(const char* s):kind(String),value(s){}
    Json(std::string s):kind(String),value(std::move(s)){}
    Json(uint64_t n):kind(Number),value(std::to_string(n)){}
    Json(bool b):kind(Bool),value(b?"true":"false"){}
    static Json array(){Json j;j.kind=Array;return j;}
    static Json object(){Json j;j.kind=Object;return j;}
    Json& operator[](const std::string& k){kind=Object;return fields[k];}
    void push(Json j){kind=Array;items.push_back(std::move(j));}
    static std::string quote(const std::string& s){
        std::ostringstream o;o<<'"';for(unsigned char c:s){
            if(c=='"'||c=='\\')o<<'\\'<<c;
            else if(c<32 || c>=127)o<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;
            else o<<c;
        }o<<'"';return o.str();
    }
    std::string dump()const{
        if(kind==Null)return "null";
        if(kind==String)return quote(value);
        if(kind==Number||kind==Bool)return value;
        std::string o=kind==Array?"[":"{";bool first=true;
        if(kind==Array)for(auto& j:items){if(!first)o+=",";first=false;o+=j.dump();}
        else for(auto& kv:fields){if(!first)o+=",";first=false;o+=quote(kv.first)+":"+kv.second.dump();}
        return o+(kind==Array?"]":"}");
    }
};
}
