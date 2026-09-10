#include "editor/SyntaxDefinition.h"

#include <QFileInfo>
#include <QString>

#include <initializer_list>

namespace ketplus {
namespace {

constexpr const char* genericKeywords =
    "as async await break case catch class const continue def do else enum export extends false "
    "finally fn for from function if import in interface let match module new nil null of package "
    "private protected public return self static struct switch this throw trait true try type "
    "typeof undefined var void while with yield";
constexpr const char* cppKeywords =
    "alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto bitand "
    "bitor bool break case catch char char8_t char16_t char32_t class compl concept const "
    "consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default "
    "delete do double dynamic_cast else enum explicit export extern false float for friend goto if "
    "inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private "
    "protected public reflexpr register reinterpret_cast requires return short signed sizeof static "
    "static_assert static_cast struct switch synchronized template this thread_local throw true try "
    "typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq";
constexpr const char* cppTypes =
    "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t size_t ptrdiff_t string "
    "string_view vector array map unordered_map set unordered_set optional variant tuple";
constexpr const char* javascriptKeywords =
    "as async await break case catch class const continue debugger default delete do else enum "
    "export extends false finally for from function get if implements import in instanceof interface "
    "keyof let namespace new null of package private protected public readonly return satisfies set "
    "static super switch this throw true try type typeof undefined var void while with yield";
constexpr const char* javaKeywords =
    "abstract assert boolean break byte case catch char class const continue default do double else "
    "enum exports extends final finally float for goto if implements import instanceof int interface "
    "long module native new non-sealed null opens package permits private protected provides public "
    "record requires return sealed short static strictfp super switch synchronized this throw throws "
    "to transient transitive true try uses var void volatile while with yield";
constexpr const char* csharpKeywords =
    "abstract as base bool break byte case catch char checked class const continue decimal default "
    "delegate do double else enum event explicit extern false finally fixed float for foreach goto "
    "if implicit in int interface internal is lock long namespace new null object operator out "
    "override params private protected public readonly record ref return sbyte sealed short sizeof "
    "stackalloc static string struct switch this throw true try typeof uint ulong unchecked unsafe "
    "ushort using virtual void volatile while async await dynamic get init partial set value var when "
    "where yield";
constexpr const char* goKeywords =
    "break case chan const continue default defer else fallthrough for func go goto if import "
    "interface map package range return select struct switch type var true false nil";
constexpr const char* swiftKeywords =
    "associatedtype break case catch class continue convenience default defer deinit do else enum "
    "extension fallthrough false fileprivate final for func get guard if import in indirect init inout "
    "internal is lazy let mutating nil nonmutating open operator override private protocol public "
    "repeat required rethrows return self set static struct subscript super switch throw throws true "
    "try typealias unowned var weak where while";
constexpr const char* kotlinKeywords =
    "as break by catch class companion const constructor continue data delegate do dynamic else enum "
    "expect external false field file final finally for fun get if import in infix init inline inner "
    "interface internal is lateinit noinline null object open operator out override package private "
    "protected public reified return sealed set suspend tailrec this throw true try typealias val var "
    "vararg when where while";
constexpr const char* pythonKeywords =
    "False None True and as assert async await break case class continue def del elif else except "
    "finally for from global if import in is lambda match nonlocal not or pass raise return try while "
    "with yield";
constexpr const char* shellKeywords =
    "break case continue do done elif else esac eval exec exit export fi for function if in local "
    "readonly return select set shift source then time trap typeset unset until while true false";
constexpr const char* sqlKeywords =
    "add all alter and any as asc backup between by case check column constraint create database "
    "default delete desc distinct drop exec exists foreign from full group having in index inner "
    "insert into is join key left like limit not null on or order outer primary procedure right rownum "
    "select set table top truncate union unique update values view where with";
constexpr const char* rustKeywords =
    "as async await break const continue crate dyn else enum extern false fn for if impl in let loop "
    "match mod move mut pub ref return self Self static struct super trait true type unsafe use where "
    "while";
constexpr const char* rustTypes =
    "bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize String Vec Option "
    "Result Box";
constexpr const char* rubyKeywords =
    "BEGIN END alias and begin break case class def defined do else elsif end ensure false for if in "
    "module next nil not or redo rescue retry return self super then true undef unless until when while "
    "yield";
constexpr const char* luaKeywords =
    "and break do else elseif end false for function goto if in local nil not or repeat return then "
    "true until while";
constexpr const char* dartKeywords =
    "abstract as assert async await base break case catch class const continue covariant default "
    "deferred do dynamic else enum export extends extension external factory false final finally for "
    "Function get hide if implements import in interface is late library mixin new null of on operator "
    "part required rethrow return sealed set show static super switch sync this throw true try typedef "
    "var void when while with yield";
constexpr const char* zigKeywords =
    "addrspace align allowzero and anyframe anytype asm async await break callconv catch comptime const "
    "continue defer else enum errdefer error export extern fn for if inline linksection noalias noinline "
    "nosuspend opaque or orelse packed pub resume return struct suspend switch test threadlocal try "
    "union unreachable usingnamespace var volatile while";
constexpr const char* htmlKeywords =
    "a abbr address article aside audio b base blockquote body br button canvas caption code col colgroup "
    "data datalist dd del details dialog div dl dt em embed fieldset figcaption figure footer form h1 "
    "h2 h3 h4 h5 h6 head header hgroup hr html i iframe img input label legend li link main map mark "
    "menu meta meter nav noscript object ol optgroup option output p picture pre progress q script "
    "section select slot small source span strong style sub summary sup table tbody td template "
    "textarea tfoot th thead time title tr track u ul video class id href src alt name type value role";
constexpr const char* phpKeywords =
    "abstract and array as break callable case catch class clone const continue declare default die do "
    "echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile enum eval exit "
    "extends final finally fn for foreach function global goto if implements include include_once "
    "instanceof insteadof interface isset list match namespace new null or print private protected "
    "public readonly require require_once return static switch throw trait true try unset use var while "
    "xor yield";
constexpr const char* cssProperties =
    "align-content align-items align-self animation appearance background border bottom box-shadow "
    "box-sizing color column-gap content cursor display fill filter flex flex-basis flex-direction "
    "flex-flow flex-grow flex-shrink flex-wrap font font-family font-size font-style font-weight gap "
    "grid grid-area grid-template-columns height inset justify-content left letter-spacing line-height "
    "margin max-height max-width min-height min-width opacity order outline overflow padding position "
    "right row-gap stroke text-align text-decoration text-overflow top transform transition visibility "
    "white-space width z-index";
constexpr const char* cmakeCommands =
    "add_compile_definitions add_executable add_library add_subdirectory cmake_minimum_required "
    "configure_file enable_testing find_package include install list message option project set "
    "set_target_properties target_compile_definitions target_compile_features target_include_directories "
    "target_link_libraries target_sources unset";
constexpr const char* cmakeParameters =
    "ALIAS ALL CACHE COMMAND COMPONENT CONFIGURE_DEPENDS DESTINATION EXCLUDE_FROM_ALL FILES GLOB "
    "INTERFACE LANGUAGES NAME OPTIONAL PRIVATE PROPERTIES PUBLIC REQUIRED SOURCES STATIC VERSION";
constexpr const char* dockerKeywords =
    "ADD ARG CMD COPY ENTRYPOINT ENV EXPOSE FROM HEALTHCHECK LABEL MAINTAINER ONBUILD RUN SHELL STOPSIGNAL "
    "USER VOLUME WORKDIR";

constexpr SyntaxDefinition genericDefinition{"generic", "cpp", {genericKeywords}};
constexpr SyntaxDefinition plainDefinition{"plain", "null", {}};
constexpr SyntaxDefinition cppDefinition{"c-cpp", "cpp", {cppKeywords, cppTypes}};
constexpr SyntaxDefinition javascriptDefinition{"javascript-typescript", "cpp",
                                                 {javascriptKeywords}};
constexpr SyntaxDefinition javaDefinition{"java", "cpp", {javaKeywords}};
constexpr SyntaxDefinition csharpDefinition{"csharp", "cpp", {csharpKeywords}};
constexpr SyntaxDefinition goDefinition{"go", "cpp", {goKeywords}};
constexpr SyntaxDefinition swiftDefinition{"swift", "cpp", {swiftKeywords}};
constexpr SyntaxDefinition kotlinDefinition{"kotlin", "cpp", {kotlinKeywords}};
constexpr SyntaxDefinition pythonDefinition{"python", "python", {pythonKeywords}};
constexpr SyntaxDefinition htmlDefinition{"html", "hypertext",
                                          {htmlKeywords, javascriptKeywords, nullptr,
                                           pythonKeywords, phpKeywords}};
constexpr SyntaxDefinition xmlDefinition{"xml", "xml", {}};
constexpr SyntaxDefinition phpDefinition{"php", "phpscript",
                                         {nullptr, nullptr, nullptr, nullptr, phpKeywords}};
constexpr SyntaxDefinition jsonDefinition{"json", "json", {"false null true"}};
constexpr SyntaxDefinition markdownDefinition{"markdown", "markdown", {}};
constexpr SyntaxDefinition cssDefinition{"css", "css", {cssProperties}};
constexpr SyntaxDefinition shellDefinition{"shell", "bash", {shellKeywords}};
constexpr SyntaxDefinition dockerDefinition{"dockerfile", "bash", {dockerKeywords}};
constexpr SyntaxDefinition yamlDefinition{"yaml", "yaml",
                                          {"false null off on true yes no"}};
constexpr SyntaxDefinition tomlDefinition{"toml", "toml", {"false true"}};
constexpr SyntaxDefinition sqlDefinition{"sql", "sql", {sqlKeywords}};
constexpr SyntaxDefinition rustDefinition{"rust", "rust", {rustKeywords, rustTypes}};
constexpr SyntaxDefinition rubyDefinition{"ruby", "ruby", {rubyKeywords}};
constexpr SyntaxDefinition luaDefinition{"lua", "lua", {luaKeywords}};
constexpr SyntaxDefinition dartDefinition{"dart", "dart", {dartKeywords}};
constexpr SyntaxDefinition zigDefinition{"zig", "zig", {zigKeywords}};
constexpr SyntaxDefinition cmakeDefinition{"cmake", "cmake",
                                           {cmakeCommands, cmakeParameters}};
constexpr SyntaxDefinition makeDefinition{"makefile", "makefile", {}};
constexpr SyntaxDefinition propertiesDefinition{"properties", "props", {}};
constexpr SyntaxDefinition diffDefinition{"diff", "diff", {}};

bool matches(const QString& value, const std::initializer_list<const char*> candidates) {
    for (const char* candidate : candidates) {
        if (value == QLatin1String(candidate)) {
            return true;
        }
    }
    return false;
}

} // namespace

const SyntaxDefinition& syntaxDefinitionForPath(const QString& filePath) {
    const QFileInfo file(filePath);
    const QString fileName = file.fileName().toLower();
    const QString suffix = file.suffix().toLower();

    if (fileName == QStringLiteral("cmakelists.txt") || suffix == QStringLiteral("cmake")) {
        return cmakeDefinition;
    }
    if (matches(fileName, {"makefile", "gnumakefile"})) {
        return makeDefinition;
    }
    if (matches(fileName, {"dockerfile", "containerfile"})) {
        return dockerDefinition;
    }
    if (fileName.startsWith(QStringLiteral("readme"))) {
        return markdownDefinition;
    }
    if (matches(fileName, {"license", "copying"}) || matches(suffix, {"txt", "log"})) {
        return plainDefinition;
    }
    if (matches(suffix, {"c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx"})) {
        return cppDefinition;
    }
    if (matches(suffix, {"js", "jsx", "mjs", "cjs", "ts", "tsx", "mts", "cts"})) {
        return javascriptDefinition;
    }
    if (suffix == QStringLiteral("java")) {
        return javaDefinition;
    }
    if (suffix == QStringLiteral("cs")) {
        return csharpDefinition;
    }
    if (suffix == QStringLiteral("go")) {
        return goDefinition;
    }
    if (suffix == QStringLiteral("swift")) {
        return swiftDefinition;
    }
    if (matches(suffix, {"kt", "kts"})) {
        return kotlinDefinition;
    }
    if (matches(suffix, {"py", "pyw"})) {
        return pythonDefinition;
    }
    if (matches(suffix, {"html", "htm", "vue", "svelte"})) {
        return htmlDefinition;
    }
    if (matches(suffix, {"xml", "svg"})) {
        return xmlDefinition;
    }
    if (suffix == QStringLiteral("php")) {
        return phpDefinition;
    }
    if (matches(suffix, {"json", "jsonc", "json5"})) {
        return jsonDefinition;
    }
    if (matches(suffix, {"md", "markdown", "mdx"})) {
        return markdownDefinition;
    }
    if (matches(suffix, {"css", "scss", "sass", "less"})) {
        return cssDefinition;
    }
    if (matches(suffix, {"sh", "bash", "zsh", "fish"})) {
        return shellDefinition;
    }
    if (matches(suffix, {"yaml", "yml"})) {
        return yamlDefinition;
    }
    if (suffix == QStringLiteral("toml")) {
        return tomlDefinition;
    }
    if (suffix == QStringLiteral("sql")) {
        return sqlDefinition;
    }
    if (suffix == QStringLiteral("rs")) {
        return rustDefinition;
    }
    if (suffix == QStringLiteral("rb")) {
        return rubyDefinition;
    }
    if (suffix == QStringLiteral("lua")) {
        return luaDefinition;
    }
    if (suffix == QStringLiteral("dart")) {
        return dartDefinition;
    }
    if (suffix == QStringLiteral("zig")) {
        return zigDefinition;
    }
    if (matches(suffix, {"ini", "cfg", "conf", "properties", "env"}) ||
        matches(fileName, {".env", ".editorconfig", ".gitconfig"})) {
        return propertiesDefinition;
    }
    if (matches(suffix, {"diff", "patch"})) {
        return diffDefinition;
    }

    return genericDefinition;
}

} // namespace ketplus
