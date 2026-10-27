// SPDX-License-Identifier: Apache-2.0
#include "material_library.hpp"
#include <miniz.h>
#include <picosha2.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <Windows.h>
#include <icu.h>
#else
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace rimes::practice::materials {
namespace fs = std::filesystem;
namespace {
[[noreturn]] void Fail(const std::string& text) { throw std::runtime_error(text); }
constexpr std::size_t kArchive = 96U * 1024 * 1024, kEntry = 24U * 1024 * 1024;
constexpr std::size_t kBook = 12U * 1024 * 1024, kText = 6U * 1024 * 1024;
std::u16string UTF16(const std::string& value) {
    if (value.size() > kEntry || value.find('\0') != std::string::npos) Fail("文本过大或包含无效字符");
    UErrorCode error = U_ZERO_ERROR; int32_t length = 0;
    u_strFromUTF8(nullptr, 0, &length, value.data(), static_cast<int32_t>(value.size()), &error);
    if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error)) Fail("EPUB 包含无效的 UTF-8 文本");
    error = U_ZERO_ERROR; std::u16string out(static_cast<std::size_t>(length), u'\0');
    u_strFromUTF8(reinterpret_cast<UChar*>(out.data()), length, nullptr, value.data(), static_cast<int32_t>(value.size()), &error);
    if (U_FAILURE(error)) Fail("无法读取 EPUB 文本");
    return out;
}
std::string UTF8(const std::u16string& value) {
    UErrorCode error = U_ZERO_ERROR; int32_t length = 0;
    u_strToUTF8(nullptr, 0, &length, reinterpret_cast<const UChar*>(value.data()), static_cast<int32_t>(value.size()), &error);
    if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error)) Fail("无效 Unicode 文本");
    error = U_ZERO_ERROR; std::string out(static_cast<std::size_t>(length), '\0');
    u_strToUTF8(out.data(), length, nullptr, reinterpret_cast<const UChar*>(value.data()), static_cast<int32_t>(value.size()), &error);
    if (U_FAILURE(error)) Fail("无效 Unicode 文本");
    return out;
}
std::string Scalar(unsigned value) {
    if (value == 0 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return {};
    std::u16string out;
    if (value <= 0xffff) out.push_back(static_cast<char16_t>(value));
    else { value -= 0x10000; out.push_back(static_cast<char16_t>(0xd800 + (value >> 10))); out.push_back(static_cast<char16_t>(0xdc00 + (value & 1023))); }
    return UTF8(out);
}
std::string DecodeXML(std::string value) {
    if (value.size() >= 2 && ((static_cast<unsigned char>(value[0]) == 255 && static_cast<unsigned char>(value[1]) == 254)
        || (static_cast<unsigned char>(value[0]) == 254 && static_cast<unsigned char>(value[1]) == 255))) {
        const bool little = static_cast<unsigned char>(value[0]) == 255;
        if (value.size() % 2) Fail("损坏的 UTF-16 EPUB 文本");
        std::u16string text;
        for (std::size_t i = 2; i < value.size(); i += 2) {
            const auto a = static_cast<unsigned char>(value[i]), b = static_cast<unsigned char>(value[i+1]);
            text.push_back(static_cast<char16_t>(little ? a + (b << 8) : (a << 8) + b));
        }
        return UTF8(text);
    }
    if (value.compare(0, 3, "\xef\xbb\xbf") == 0) value.erase(0, 3);
    (void)UTF16(value); return value;
}
std::string Entity(const std::string& input) {
    std::string out;
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] == '&') {
            auto end = input.find(';', i + 1);
            if (end != std::string::npos && end-i <= 16) {
                auto name = input.substr(i+1, end-i-1); std::string decoded;
                if (name == "amp") decoded = "&"; else if (name == "lt") decoded = "<";
                else if (name == "gt") decoded = ">"; else if (name == "quot") decoded = "\"";
                else if (name == "apos") decoded = "'"; else if (name == "nbsp") decoded = " ";
                else if (name.size() > 1 && name[0] == '#') {
                    try { std::size_t used = 0; const bool hex = name.size()>2 && (name[1]=='x' || name[1]=='X');
                        const auto number = name.substr(hex?2:1); auto code = std::stoul(number, &used, hex?16:10);
                        if (used == number.size() && code <= 0x10ffff) decoded = Scalar(static_cast<unsigned>(code));
                    } catch (const std::exception&) { }
                }
                if (!decoded.empty()) { out += decoded; i = end+1; continue; }
            }
        }
        out += input[i++];
    }
    return out;
}
std::string Lower(std::string value) { for (auto& c:value) if (static_cast<unsigned char>(c)<128) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return value; }
struct Tag { std::string name; std::map<std::string,std::string> attrs; bool close=false; };
Tag ParseTag(std::string text) {
    Tag out; std::size_t i=0;
    while (i<text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i<text.size() && text[i]=='/') { out.close=true; ++i; }
    auto start=i; while (i<text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i]!='/') ++i;
    out.name=Lower(text.substr(start,i-start)); const auto colon=out.name.find(':'); if (colon!=std::string::npos) out.name.erase(0,colon+1);
    while (i<text.size()) {
        while (i<text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i]=='/')) ++i;
        start=i; while(i<text.size() && text[i]!='=' && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        std::string key=Lower(text.substr(start,i-start));
        while(i<text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i>=text.size() || text[i]!='=') { if(i<text.size()) ++i; continue; }
        ++i; while(i<text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if(i>=text.size()) break;
        const char quote=text[i]=='\'' || text[i]=='"'?text[i++]:0; start=i;
        while(i<text.size() && (quote?text[i]!=quote:!std::isspace(static_cast<unsigned char>(text[i])) && text[i]!='/')) ++i;
        out.attrs.emplace(std::move(key),Entity(text.substr(start,i-start))); if(quote && i<text.size()) ++i;
    }
    return out;
}
struct Piece { Tag tag; std::string text; bool markup=false; };
std::vector<Piece> Pieces(const std::string& xml) {
    std::vector<Piece> out; std::size_t i=0;
    while(i<xml.size()) {
        if(xml.compare(i,4,"<!--")==0) { auto end=xml.find("-->",i+4); if(end==std::string::npos) break; i=end+3;continue; }
        if(xml[i]!='<') { auto end=xml.find('<',i); if(end==std::string::npos)end=xml.size();out.push_back({{},Entity(xml.substr(i,end-i)),false});i=end;continue; }
        if(xml.compare(i,9,"<![CDATA[")==0) { auto end=xml.find("]]>",i+9); if(end==std::string::npos)break;out.push_back({{},xml.substr(i+9,end-i-9),false});i=end+3;continue; }
        auto end=i+1;char quote=0;
        for(;end<xml.size();++end) { char c=xml[end];if(quote) {if(c==quote)quote=0;}else if(c=='\'' || c=='"')quote=c;else if(c=='>')break; }
        if(end==xml.size())break;
        out.push_back({ParseTag(xml.substr(i+1,end-i-1)),{},true});i=end+1;
    }
    return out;
}
std::string Tidy(const std::string& text) {
    std::string out; bool space=false;
    for (const auto& c:Characters(text)) {
        auto u=UTF16(c); if (!u.empty() && u_isUWhiteSpace(static_cast<UChar32>(u[0]))) { space=!out.empty();continue; }
        if(space)out+=' ';space=false;out+=c;
    }
    return out;
}
std::vector<std::string> Paragraphs(const std::string& html) {
    std::vector<std::string> out;std::string current;int skipped=0;
    auto flush=[&] { auto text=Tidy(current);current.clear();if(!text.empty())out.push_back(std::move(text)); };
    const std::set<std::string> blocks={"p","div","section","h1","h2","h3","h4","h5","h6","li","br","blockquote","hr"};
    const std::set<std::string> hidden={"head","script","style","nav","svg","h1","h2","h3","h4","h5","h6"};
    for (const auto& piece:Pieces(html)) {
        if (piece.markup) {
            if (hidden.count(piece.tag.name)) { if(piece.tag.close) skipped=std::max(0,skipped-1);else ++skipped;continue; }
            if(!skipped && blocks.count(piece.tag.name))flush();
        } else if(!skipped) current+=piece.text;
    }
    flush();return out;
}
std::string Heading(const std::string& html) {
    std::string current;bool inside=false;
    for(const auto& piece:Pieces(html)) {
        if(piece.markup && piece.tag.name.size()==2 && piece.tag.name[0]=='h' && piece.tag.name[1]>='1' && piece.tag.name[1]<='6') {
            if(piece.tag.close && inside)return Tidy(current);
            if(!piece.tag.close)inside=true;
        } else if(inside && !piece.markup)current+=piece.text;
    }
    return {};
}
std::string Clean(const std::string& text) {
    std::string out,last; const std::set<std::string> decorative={"※","★","☆","◆","◇","■","□","●","○","◎","◈","▲","△","▼","▽","♦","♠","♥","♣","▶","◀","►","◄","〓","§","¶","†","‡"};
    const std::set<std::string> repeated={"，","。","！","？","；","：","、",",",".","!","?",";",":","-","_","=","*","~","～","·"};
    auto chars=Characters(Tidy(text));
    for(std::size_t i=0;i<chars.size();++i) {
        auto c=chars[i]; if(decorative.count(c))continue;
        if(c=="[" || c=="［" || c=="【" || c=="(" || c=="（") {
            std::size_t end=i+1;int digits=0;bool valid=true;
            for(;end<chars.size() && end-i<12;++end) {
                auto t=chars[end]; if(t=="]" || t=="］" || t=="】" || t==")" || t=="）")break;
                if(t.size()==1 && t[0]>='0' && t[0]<='9')++digits;else if(t!=" " && t!="注")valid=false;
            }
            if(valid && digits>0 && digits<=3 && end<chars.size() && end-i<12) {i=end;continue;}
        }
        if(c==last && repeated.count(c))continue;
        last=c;out+=c;
    }
    // Links and addresses are not useful practice material. Scan bounded tokens,
    // never run a backtracking regex over long unspaced Chinese paragraphs.
    std::string cleaned;
    for(std::size_t i=0;i<out.size();) {
        auto end=out.find(' ',i);if(end==std::string::npos)end=out.size();auto token=out.substr(i,end-i);
        auto at=token.find('@');
        if(token.find("http://")==std::string::npos && token.find("https://")==std::string::npos && token.find("www.")==std::string::npos && !(at!=std::string::npos && token.find('.',at)!=std::string::npos)) {
            if(!cleaned.empty())cleaned+=' ';cleaned+=token;
        }
        i=end+1;
    }
    return Tidy(cleaned);
}
std::string Path(std::string value) {
    if(value.empty() || value[0]=='/' || value.find('\\')!=std::string::npos || value.find(':')!=std::string::npos || value.find('\0')!=std::string::npos)Fail("EPUB 包含不安全的路径");
    fs::path result;
    for(const auto& component:fs::u8path(value)) {
        const auto part=component.u8string();if(part=="..") {if(result.empty())Fail("EPUB 路径超出包目录");result=result.parent_path();}else if(part!=".")result/=component;
    }
    return result.generic_u8string();
}
std::string Resolve(const std::string& base,std::string href) {
    href=href.substr(0,href.find_first_of("#?"));std::string decoded;
    for(std::size_t i=0;i<href.size();++i) {
        if(href[i]=='%' && i+2<href.size()) {try {std::size_t used=0;auto c=std::stoul(href.substr(i+1,2),&used,16);if(used==2){decoded+=static_cast<char>(c);i+=2;continue;}}catch(const std::exception&){} }
        decoded+=href[i];
    }
    return Path(base+decoded);
}
class Archive {
 public:
    explicit Archive(const fs::path& path) {
        if(fs::is_symlink(fs::symlink_status(path)) || !fs::is_regular_file(path) || fs::file_size(path)>kArchive)Fail("请选择不超过 96 MiB 的 EPUB 文件");
        const auto size=fs::file_size(path); bytes.resize(static_cast<std::size_t>(size));
        std::ifstream file(path,std::ios::binary);file.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        if(bytes.empty() || file.gcount()!=static_cast<std::streamsize>(bytes.size()) || file.peek()!=std::char_traits<char>::eof())Fail("无法完整读取 EPUB 文件");
        if(!mz_zip_reader_init_mem(&zip,bytes.data(),bytes.size(),0))Fail("EPUB ZIP 已损坏");
        opened=true;
        try {
            if(zip.m_total_files>20000)Fail("EPUB 条目过多");std::uint64_t total=0;
            for(mz_uint i=0;i<zip.m_total_files;++i) {
                mz_zip_archive_file_stat st{};if(!mz_zip_reader_file_stat(&zip,i,&st))Fail("无法读取 EPUB 目录");
                if(st.m_is_directory)continue;
                auto length=mz_zip_reader_get_filename(&zip,i,nullptr,0);if(length<2 || length>4096)Fail("EPUB 路径过长");
                std::string name(length,'\0');mz_zip_reader_get_filename(&zip,i,name.data(),length);name.pop_back();auto normalized=Path(name);
                if(normalized!=name || !entries.emplace(name,i).second || st.m_is_encrypted || (((st.m_external_attr>>16)&0170000U)==0120000U))Fail("EPUB 路径重复、加密或含符号链接");
                total+=st.m_uncomp_size;if(st.m_uncomp_size>kEntry || total>128U*1024*1024)Fail("EPUB 展开大小超过限制");
            }
        } catch(...) {mz_zip_reader_end(&zip);opened=false;throw;}
    }
    ~Archive() {if(opened)mz_zip_reader_end(&zip);}
    bool Has(const std::string& name) const {return entries.count(name)!=0;}
    std::string Read(const std::string& name) {
        auto found=entries.find(name);if(found==entries.end())Fail("EPUB 缺少文件："+name);
        mz_zip_archive_file_stat st{};if(!mz_zip_reader_file_stat(&zip,found->second,&st) || st.m_uncomp_size>kEntry)Fail("EPUB 文件过大");
        std::string text(static_cast<std::size_t>(st.m_uncomp_size),'\0');
        if(!mz_zip_reader_extract_to_mem(&zip,found->second,text.data(),text.size(),0))Fail("EPUB 内容或 CRC 损坏");
        return DecodeXML(std::move(text));
    }
    std::string bytes;
 private:
    mz_zip_archive zip{};bool opened=false;std::map<std::string,mz_uint> entries;
};
bool Wide(const std::string& text) {
    for(auto c:UTF16(text))if((c>=0x2e80 && c<=0x9fff) || (c>=0xf900 && c<=0xfaff))return true;
    return false;
}
std::string Join(const std::vector<std::string>& text,std::size_t start,std::size_t end) {std::string result;for(auto i=start;i<end;++i)result+=text[i];return result;}
bool ValidID(const std::string& id) {return id.size()==16 && id.find_first_not_of("0123456789abcdef")==std::string::npos;}
void Directory(const fs::path& path) {
    if(fs::is_symlink(fs::symlink_status(path)))Fail("文库目录不能是符号链接");
    fs::create_directories(path);
#ifndef _WIN32
    fs::permissions(path,fs::perms::owner_all,fs::perm_options::replace);
#endif
}
class Lock {
 public:
    explicit Lock(const fs::path& path) {
#ifdef _WIN32
        handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)Fail("文库正被其他窗口修改，请稍后重试");
#else
        descriptor=open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        if(descriptor<0 || flock(descriptor,LOCK_EX|LOCK_NB)!=0) {if(descriptor>=0)close(descriptor);descriptor=-1;Fail("文库正被其他窗口修改，请稍后重试");}
#endif
    }
    ~Lock() {
#ifdef _WIN32
        CloseHandle(handle);
#else
        if(descriptor>=0)close(descriptor);
#endif
    }
 private:
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
#else
    int descriptor=-1;
#endif
};
void ValidateBook(const Json& value) {
    if(!value.is_object() || value.value("version",0)!=1 || !ValidID(value.value("id",std::string{})) || !value.contains("chapters") || !value.at("chapters").is_array() || value.at("chapters").empty())Fail("文库文件无效，原文件已保留");
    std::size_t count=0,characters=0;
    for(const auto& chapter:value.at("chapters")) {
        if(!chapter.is_object() || !chapter.at("title").is_string() || !chapter.at("passages").is_array())Fail("文库章节无效");
        for(const auto& text:chapter.at("passages")) {if(!text.is_string())Fail("文库正文无效");characters+=text.get_ref<const std::string&>().size();if(++count>6000 || characters>kBook)Fail("文库超过限制");}
    }
}
}
std::vector<std::string> Characters(const std::string& text) {
    auto u=UTF16(text);UErrorCode error=U_ZERO_ERROR;
    auto* iterator=ubrk_open(UBRK_CHARACTER,"root",reinterpret_cast<const UChar*>(u.data()),static_cast<int32_t>(u.size()),&error);
    if(U_FAILURE(error) || !iterator)Fail("无法按字符读取文本");
    std::vector<std::string> out;int32_t begin=ubrk_first(iterator);
    for(auto end=ubrk_next(iterator);end!=UBRK_DONE;begin=end,end=ubrk_next(iterator))out.push_back(UTF8(u.substr(static_cast<std::size_t>(begin),static_cast<std::size_t>(end-begin))));
    ubrk_close(iterator);return out;
}
std::string ScoredText(const std::string& text) {std::string out;for(const auto& c:Characters(text))if(c!="\n" && c!="\r" && c!="\r\n")out+=c;return out;}
std::vector<TrackRow> TrackRows(const std::string& text,int columns,int indent) {
    columns=std::clamp(columns,4,200);indent=std::clamp(indent,0,columns-1);
    std::vector<TrackRow> rows;TrackRow row;row.indent=indent;std::size_t offset=0;
    auto flush=[&] {if(!row.text.empty()){rows.push_back(row);row={};row.start=offset;}};
    for(const auto& c:Characters(text)) {
        if(c=="\n" || c=="\r" || c=="\r\n") {flush();row.indent=indent;continue;}
        if(row.length>=static_cast<std::size_t>(columns-row.indent)){flush();row.indent=0;}
        row.text+=c;++row.length;++offset;
    }
    flush();return rows;
}
std::vector<std::string> Passages(const std::vector<std::string>& paragraphs) {
    std::string all;for(const auto& p:paragraphs)all+=p;const bool wide=Wide(all);
    const std::size_t limit=wide?800:1000;const std::string separator=wide?"\n":" ";const std::size_t gap=wide?0:1;
    std::vector<std::string> result;std::string current;std::size_t currentCount=0;
    auto emit=[&](const std::string& value){auto trimmed=Tidy(value);if(!trimmed.empty())result.push_back(value);};
    auto flush=[&]{emit(current);current.clear();currentCount=0;};
    for(const auto& paragraph:paragraphs) {
        auto chars=Characters(paragraph);
        if(chars.size()>limit) {
            flush();const auto parts=(chars.size()+limit-1)/limit;const auto target=(chars.size()+parts-1)/parts;
            std::vector<std::string> sentences;std::string sentence;
            for(const auto& c:chars) {sentence+=c;if(c=="。" || c=="！" || c=="？" || c=="!" || c=="?" || c=="."){sentences.push_back(sentence);sentence.clear();}}
            if(!sentence.empty())sentences.push_back(sentence);
            std::string part;std::size_t partCount=0;
            for(const auto& value:sentences) {auto tokens=Characters(value);for(std::size_t start=0;start<tokens.size();start+=limit){auto end=std::min(tokens.size(),start+limit);auto size=end-start;if(partCount>=target || partCount+size>limit){emit(part);part.clear();partCount=0;}part+=Join(tokens,start,end);partCount+=size;}}
            emit(part);continue;
        }
        if(!current.empty() && currentCount+gap+chars.size()>limit)flush();
        if(!current.empty()){current+=separator;currentCount+=gap;}current+=paragraph;currentCount+=chars.size();
    }
    flush();
    if(result.size()>1) {auto last=Characters(ScoredText(result.back())).size();auto previous=Characters(ScoredText(result[result.size()-2])).size();if(last<150 && previous+last+gap<=limit+limit/6){auto value=result.back();result.pop_back();result.back()+=separator+value;}}
    return result;
}
Json ParseEPUB(const fs::path& path) {
    Archive archive(path);std::string package;
    for(const auto& piece:Pieces(archive.Read("META-INF/container.xml")))if(piece.markup && piece.tag.name=="rootfile"){auto it=piece.tag.attrs.find("full-path");if(it!=piece.tag.attrs.end()){package=Path(it->second);break;}}
    if(package.empty())Fail("EPUB 缺少 OPF 包文件");
    auto slash=package.find_last_of('/');const std::string base=slash==std::string::npos?"":package.substr(0,slash+1);
    struct Item {std::string href,type,properties;};std::map<std::string,Item> manifest;std::vector<std::string> spine;std::string title,author,field;
    for(const auto& piece:Pieces(archive.Read(package))) {
        if(piece.markup) {
            auto tag=piece.tag;
            if(tag.name=="item" && !tag.close)manifest.emplace(tag.attrs["id"],Item{tag.attrs["href"],tag.attrs["media-type"],tag.attrs["properties"]});
            if(tag.name=="itemref" && !tag.close && tag.attrs["linear"]!="no")spine.push_back(tag.attrs["idref"]);
            if(tag.name=="title" || tag.name=="creator")field=tag.close?"":tag.name;
        } else if(field=="title")title+=piece.text;else if(field=="creator")author+=piece.text;
    }
    if(spine.empty())Fail("EPUB 没有可阅读的章节");
    Json chapters=Json::array();std::size_t total=0,count=0;
    for(const auto& id:spine) {
        auto item=manifest.find(id);if(item==manifest.end() || item->second.type.find("html")==std::string::npos || item->second.properties.find("nav")!=std::string::npos)continue;
        auto name=Resolve(base,item->second.href);if(!archive.Has(name))continue;
        auto document=archive.Read(name);auto heading=Heading(document);
        auto lower=Lower(heading);if(lower.find("copyright")!=std::string::npos || lower.find("contents")!=std::string::npos || heading=="目录" || heading.find("版权")!=std::string::npos)continue;
        auto raw=Paragraphs(document);std::vector<std::string> paragraphs;
        for(const auto& value:raw) {auto cleaned=Clean(value);auto chars=Characters(cleaned);std::size_t letters=0;for(auto c:UTF16(cleaned))if(u_isalpha(static_cast<UChar32>(c)))++letters;if(letters>=4 && letters*100>=chars.size()*65)paragraphs.push_back(std::move(cleaned));}
        std::string joined;for(const auto& value:paragraphs)joined+=value;
        if(Characters(joined).size()<30)continue;
        auto passages=Passages(paragraphs);if(passages.empty())continue;
        total+=Characters(joined).size();count+=passages.size();if(total>kText || count>6000)Fail("EPUB 正文超过限制");
        auto chars=Characters(heading);if(chars.size()>40)heading=Join(chars,0,40);
        auto number=std::to_string(chapters.size()+1);number=std::string(number.size()<3?3-number.size():0,'0')+number;
        chapters.push_back({{"title",number+" · "+(heading.empty()?"正文":heading)},{"passages",passages}});
    }
    if(chapters.empty())Fail("没有从 EPUB 读出可供跟打的正文");
    const auto digest=picosha2::hash256_hex_string(archive.bytes);
    if(title.empty())title=path.stem().u8string();
    Json book={{"version",1},{"cut",4},{"id",digest.substr(0,16)},{"title",Tidy(title)},{"author",Tidy(author)},
        {"importedAt",std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()},{"chapters",chapters}};
    if(book.dump().size()>kBook)Fail("书籍超过本机文库大小限制");ValidateBook(book);return book;
}
std::vector<Json> Articles(const Json& book) {
    ValidateBook(book);std::vector<Json> values;std::size_t index=0;
    for(const auto& chapter:book.at("chapters")) {
        std::size_t part=0;for(const auto& text:chapter.at("passages")) {
            auto display=text.get<std::string>();auto scored=ScoredText(display);bool wide=Wide(scored);std::size_t words=0;bool inWord=false;
            for(auto c:UTF16(scored)){bool space=u_isUWhiteSpace(static_cast<UChar32>(c));if(!space && !inWord)++words;inWord=!space;}
            auto title=book.at("title").get<std::string>()+" · "+chapter.at("title").get<std::string>();
            if(chapter.at("passages").size()>1)title+=" · "+std::to_string(++part)+"/"+std::to_string(chapter.at("passages").size());
            values.push_back({{"id","epub."+book.at("id").get<std::string>()+"."+std::to_string(++index)},
                {"version",book.value("cut",1)},{"language",wide?"chinese":"english"},{"title",title},{"theme",book.value("author",std::string{}).empty()?book.at("title"):book.at("author")},
                {"difficulty","导入"},{"text",scored},{"displayText",display},{"characterCount",Characters(scored).size()},{"wordCount",words}});
        }
    }
    return values;
}
Library::Library(fs::path user_root):root_(std::move(user_root)/"typing-library") {}
std::vector<Json> Library::Books() const {
    if(!fs::exists(root_))return {};
    if(fs::is_symlink(fs::symlink_status(root_)) || !fs::is_directory(root_))Fail("文库目录无效");
    std::vector<Json> values;
    for(const auto& entry:fs::directory_iterator(root_)) {
        if(entry.path().extension()!=".json")continue;
        if(values.size()>=40 || entry.is_symlink() || !entry.is_regular_file() || entry.file_size()>kBook)Fail("文库文件过大或不安全，原文件已保留");
        std::ifstream file(entry.path(),std::ios::binary);Json book;file>>book;ValidateBook(book);
        if(entry.path().stem().u8string()!=book.at("id").get<std::string>())Fail("文库身份不符，原文件已保留");values.push_back(std::move(book));
    }
    std::sort(values.begin(),values.end(),[](const Json& a,const Json& b){return a.value("importedAt",0.)>b.value("importedAt",0.);});return values;
}
std::vector<Json> Library::Articles() const {std::vector<Json> values;for(const auto& book:Books()){auto next=materials::Articles(book);values.insert(values.end(),next.begin(),next.end());}return values;}
Json Library::Import(const fs::path& path) {
    auto book=ParseEPUB(path);Directory(root_);Lock lock(root_/".write-lock");
    auto books=Books();const auto id=book.at("id").get<std::string>();
    if(books.size()>=40 && std::none_of(books.begin(),books.end(),[&](const Json& b){return b.at("id")==id;}))Fail("文库已达 40 本上限");
    auto destination=root_/fs::u8path(id+".json");if(fs::is_symlink(fs::symlink_status(destination)))Fail("文库文件不能是符号链接");
    auto temporary=root_/fs::u8path(id+".new");if(fs::exists(fs::symlink_status(temporary)))Fail("文库暂存文件已存在，请检查后重试");
    try {
        const auto bytes=book.dump(2);std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));file.close();if(!file)Fail("无法保存文库");
#ifndef _WIN32
        fs::permissions(temporary,fs::perms::owner_read|fs::perms::owner_write,fs::perm_options::replace);
        fs::rename(temporary,destination);
#else
        if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))Fail("无法原子更新文库");
#endif
    } catch(...) {std::error_code ignored;fs::remove(temporary,ignored);throw;}
    return book;
}
void Library::Remove(const std::string& id) {if(!ValidID(id))Fail("无效书籍身份");Directory(root_);Lock lock(root_/".write-lock");auto path=root_/fs::u8path(id+".json");if(fs::is_symlink(fs::symlink_status(path)))Fail("文库文件不能是符号链接");fs::remove(path);}
}
