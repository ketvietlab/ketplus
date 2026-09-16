#include "editor/SyntaxDefinition.h"

#include <QFileInfo>
#include <QString>

#include <algorithm>
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

constexpr const char* perlKeywords =
    "__DATA__ __END__ __FILE__ __LINE__ __PACKAGE__ and chomp chop cmp continue default defined "
    "delete die do each else elsif eq eval exists exit for foreach ge given gt if keys last le "
    "local lt map my ne next no not or our package pop print printf push redo ref require return "
    "say scalar shift sort splice split sprintf state sub undef unless unshift until use values "
    "wantarray when while xor";
constexpr const char* rKeywords =
    "if else repeat while function for next break TRUE FALSE NULL Inf NaN NA NA_integer_ NA_real_ "
    "NA_character_ in return library require";
constexpr const char* batchKeywords =
    "rem set if exist errorlevel for in do break call chcp cd chdir choice cls country ctty date "
    "del erase dir echo exit goto loadfix loadhigh mkdir md move path pause prompt rename ren rmdir "
    "rd shift time type ver verify vol com con lpt nul not else endlocal setlocal defined pushd "
    "popd start title";
constexpr const char* powershellKeywords =
    "begin break catch class continue data default do dynamicparam else elseif end enum exit "
    "filter finally for foreach from function hidden if in param process return static switch "
    "throw trap try until using var while";
constexpr const char* haskellKeywords =
    "case class data default deriving do else foreign if import in infix infixl infixr instance "
    "let module newtype of then type where forall mdo proc rec";
constexpr const char* erlangKeywords =
    "after and andalso band begin bnot bor bsl bsr bxor case catch cond div end fun if let not of "
    "or orelse query receive rem try when xor";
constexpr const char* lispKeywords =
    "defun defmacro defvar defparameter defconstant defn def let let* lambda if cond when unless "
    "progn setq setf loop do dolist dotimes quote function and or not ns require fn loop recur";
constexpr const char* pascalKeywords =
    "and array as asm begin case class const constructor destructor div do downto else end except "
    "exports file finalization finally for function goto if implementation in inherited "
    "initialization inline interface is label library mod nil not object of or out packed "
    "procedure program property raise record repeat set shl shr string then threadvar to try type "
    "unit until uses var while with xor private protected public published override virtual";
constexpr const char* fortranKeywords =
    "allocatable allocate assign associate call case character class close common complex contains "
    "continue cycle data deallocate default dimension do double else elseif elsewhere end enddo "
    "endif entry exit external format function go goto if implicit in include inout integer intent "
    "interface intrinsic logical module namelist none nullify only open optional out parameter "
    "pointer precision print private procedure program public read real recursive result return "
    "save select sequence stop subroutine target then type use where while write";
constexpr const char* tclKeywords =
    "after append array break case catch cd close concat continue default else elseif eof error "
    "eval exec exit expr file for foreach format gets glob global if incr info join lappend lindex "
    "list llength lrange lreplace lsearch lsort namespace open package proc puts pwd read regexp "
    "regsub rename return set source split string switch uplevel upvar variable while";
constexpr const char* vbKeywords =
    "addhandler addressof alias and andalso as boolean byref byte byval call case catch cbool "
    "cbyte cchar cdate cdbl cdec char cint class clng cobj const continue csbyte cshort csng cstr "
    "ctype cuint culng cushort date decimal declare default delegate dim directcast do double each "
    "else elseif end endif enum erase error event exit false finally for friend function get "
    "gettype global gosub goto handles if implements imports in inherits integer interface is isnot "
    "let lib like long loop me mod module mustinherit mustoverride mybase myclass namespace narrowing "
    "new next not nothing notinheritable notoverridable object of on operator option optional or "
    "orelse overloads overridable overrides paramarray partial private property protected public "
    "raiseevent readonly redim rem removehandler resume return select set shadows shared short "
    "single static step stop string structure sub synclock then throw to true try trycast typeof "
    "uinteger ulong ushort using variant wend when while widening with withevents writeonly xor";
constexpr const char* dKeywords =
    "abstract alias align asm assert auto body bool break byte case cast catch cdouble cent cfloat "
    "char class const continue creal dchar debug default delegate delete deprecated do double else "
    "enum export extern false final finally float for foreach foreach_reverse function goto idouble "
    "if ifloat immutable import in inout int interface invariant ireal is lazy long mixin module new "
    "nothrow null out override package pragma private protected public pure real ref return scope "
    "shared short static struct super switch synchronized template this throw true try typeid "
    "typeof ubyte ucent uint ulong union unittest ushort version void wchar while with";
constexpr const char* juliaKeywords =
    "baremodule begin break catch const continue do else elseif end export false finally for "
    "function global if import let local macro module quote return struct true try using while "
    "abstract mutable primitive type where in isa";
constexpr const char* nimKeywords =
    "addr and as asm bind block break case cast concept const continue converter defer discard "
    "distinct div do elif else end enum except export finally for from func if import in include "
    "interface is isnot iterator let macro method mixin mod nil not notin object of or out proc ptr "
    "raise ref return shl shr static template try tuple type using var when while xor yield";
constexpr const char* coffeescriptKeywords =
    "and break by catch class continue debugger delete do else extends false finally for if in "
    "instanceof is isnt loop new no not null of off on or return super switch then this throw true "
    "try typeof undefined unless until when while yes yield";
constexpr const char* verilogKeywords =
    "always and assign automatic begin buf case casex casez cell config deassign default defparam "
    "design disable edge else end endcase endconfig endfunction endgenerate endmodule endprimitive "
    "endspecify endtable endtask event for force forever fork function generate genvar if initial "
    "inout input integer join localparam logic macromodule module nand negedge nor not or output "
    "parameter posedge reg release repeat signed specify supply0 supply1 table task time tri "
    "unsigned wait while wire xnor xor";
constexpr const char* vhdlKeywords =
    "abs access after alias all and architecture array assert attribute begin block body buffer bus "
    "case component configuration constant disconnect downto else elsif end entity exit file for "
    "function generate generic group guarded if impure in inertial inout is label library linkage "
    "literal loop map mod nand new next nor not null of on open or others out package port "
    "postponed procedure process pure range record register reject rem report return rol ror "
    "select severity signal shared sla sll sra srl subtype then to transport type unaffected units "
    "until use variable wait when while with xnor xor";
constexpr const char* ocamlKeywords =
    "and as assert asr begin class constraint do done downto else end exception external false for "
    "fun function functor if in include inherit initializer land lazy let lor lsl lsr lxor match "
    "method mod module mutable new nonrec object of open or private rec sig struct then to true try "
    "type val virtual when while with";
constexpr const char* fsharpKeywords =
    "abstract and as assert base begin class default delegate do done downcast downto elif else end "
    "exception extern false finally fixed for fun function global if in inherit inline interface "
    "internal lazy let match member module mutable namespace new not null of open or override "
    "private public rec return select sig static struct then to true try type upcast use val void "
    "when while with yield async task";
constexpr const char* gdscriptKeywords =
    "and as assert await break breakpoint class class_name const continue elif else enum export "
    "extends false for func if in is match not null or pass preload return self signal static super "
    "true var void while yield onready tool";
constexpr const char* asmInstructions =
    "adc add and call cmp dec div hlt idiv imul inc int ja jae jb jbe je jg jge jl jle jmp jne jnz "
    "jz lea leave loop mov movsx movzx mul neg nop not or pop push ret sal sar shl shr sub test xchg "
    "xor syscall";
constexpr const char* asmRegisters =
    "al ah ax eax rax bl bh bx ebx rbx cl ch cx ecx rcx dl dh dx edx rdx si esi rsi di edi rdi sp esp "
    "rsp bp ebp rbp r8 r9 r10 r11 r12 r13 r14 r15 cs ds es fs gs ss";
constexpr const char* asmDirectives =
    "section segment global extern db dw dd dq resb resw resd resq equ times org bits align macro "
    "endm proc endp include";
constexpr const char* scalaKeywords =
    "abstract case catch class def do else enum export extends false final finally for forSome "
    "given if implicit import lazy match new null object override package private protected return "
    "sealed super then this throw trait true try type using val var while with yield";
constexpr const char* groovyKeywords =
    "abstract as assert boolean break byte case catch char class const continue def default do "
    "double else enum extends false final finally float for goto if implements import in instanceof "
    "int interface long native new null package private protected public return short static super "
    "switch synchronized this throw throws trait transient true try var void volatile while";
constexpr const char* objcKeywords =
    "auto break case char const continue default do double else enum extern float for goto if "
    "inline int long register restrict return short signed sizeof static struct switch typedef "
    "union unsigned void volatile while @interface @implementation @end @protocol @property "
    "@synthesize @dynamic @selector @class @public @private @protected @try @catch @finally "
    "@autoreleasepool id self super nil YES NO BOOL instancetype nonatomic strong weak copy assign "
    "readonly readwrite";
constexpr const char* protoKeywords =
    "syntax package import option message enum service rpc returns stream repeated optional "
    "required oneof map reserved extend extensions to max true false double float int32 int64 "
    "uint32 uint64 sint32 sint64 fixed32 fixed64 sfixed32 sfixed64 bool string bytes";

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
constexpr SyntaxDefinition scssDefinition{"scss", "css", {cssProperties}};
constexpr SyntaxDefinition lessDefinition{"less", "css", {cssProperties}};
constexpr SyntaxDefinition perlDefinition{"perl", "perl", {perlKeywords}};
constexpr SyntaxDefinition rDefinition{"r", "r", {rKeywords}};
constexpr SyntaxDefinition batchDefinition{"batch", "batch", {batchKeywords}};
constexpr SyntaxDefinition powershellDefinition{"powershell", "powershell",
                                                {powershellKeywords}};
constexpr SyntaxDefinition haskellDefinition{"haskell", "haskell", {haskellKeywords}};
constexpr SyntaxDefinition erlangDefinition{"erlang", "erlang", {erlangKeywords}};
constexpr SyntaxDefinition lispDefinition{"lisp", "lisp", {lispKeywords}};
constexpr SyntaxDefinition pascalDefinition{"pascal", "pascal", {pascalKeywords}};
constexpr SyntaxDefinition fortranDefinition{"fortran", "fortran", {fortranKeywords}};
constexpr SyntaxDefinition tclDefinition{"tcl", "tcl", {tclKeywords}};
constexpr SyntaxDefinition vbDefinition{"visual-basic", "vb", {vbKeywords}};
constexpr SyntaxDefinition dDefinition{"d", "d", {dKeywords}};
constexpr SyntaxDefinition juliaDefinition{"julia", "julia", {juliaKeywords}};
constexpr SyntaxDefinition nimDefinition{"nim", "nim", {nimKeywords}};
constexpr SyntaxDefinition latexDefinition{"latex", "latex", {}};
constexpr SyntaxDefinition asmDefinition{"assembly", "asm",
                                         {asmInstructions, nullptr, asmRegisters, asmDirectives}};
constexpr SyntaxDefinition coffeescriptDefinition{"coffeescript", "coffeescript",
                                                  {coffeescriptKeywords}};
constexpr SyntaxDefinition verilogDefinition{"verilog", "verilog", {verilogKeywords}};
constexpr SyntaxDefinition vhdlDefinition{"vhdl", "vhdl", {vhdlKeywords}};
constexpr SyntaxDefinition ocamlDefinition{"ocaml", "caml", {ocamlKeywords}};
constexpr SyntaxDefinition fsharpDefinition{"fsharp", "fsharp", {fsharpKeywords}};
constexpr SyntaxDefinition gdscriptDefinition{"gdscript", "gdscript", {gdscriptKeywords}};
constexpr SyntaxDefinition scalaDefinition{"scala", "cpp", {scalaKeywords}};
constexpr SyntaxDefinition groovyDefinition{"groovy", "cpp", {groovyKeywords}};
constexpr SyntaxDefinition objcDefinition{"objective-c", "cpp", {objcKeywords, cppTypes}};
constexpr SyntaxDefinition protoDefinition{"protobuf", "cpp", {protoKeywords}};

struct NamedDefinition final {
    const SyntaxDefinition* definition;
    const char* displayName;
};

constexpr NamedDefinition namedDefinitions[] = {
    {&genericDefinition, "Generic Code"},
    {&plainDefinition, "Plain Text"},
    {&cppDefinition, "C / C++"},
    {&javascriptDefinition, "JavaScript / TypeScript"},
    {&javaDefinition, "Java"},
    {&csharpDefinition, "C#"},
    {&goDefinition, "Go"},
    {&swiftDefinition, "Swift"},
    {&kotlinDefinition, "Kotlin"},
    {&pythonDefinition, "Python"},
    {&htmlDefinition, "HTML"},
    {&xmlDefinition, "XML"},
    {&phpDefinition, "PHP"},
    {&jsonDefinition, "JSON"},
    {&markdownDefinition, "Markdown"},
    {&cssDefinition, "CSS"},
    {&shellDefinition, "Shell"},
    {&dockerDefinition, "Dockerfile"},
    {&yamlDefinition, "YAML"},
    {&tomlDefinition, "TOML"},
    {&sqlDefinition, "SQL"},
    {&rustDefinition, "Rust"},
    {&rubyDefinition, "Ruby"},
    {&luaDefinition, "Lua"},
    {&dartDefinition, "Dart"},
    {&zigDefinition, "Zig"},
    {&cmakeDefinition, "CMake"},
    {&makeDefinition, "Makefile"},
    {&propertiesDefinition, "INI / Properties"},
    {&diffDefinition, "Diff"},
    {&scssDefinition, "SCSS"},
    {&lessDefinition, "Less"},
    {&perlDefinition, "Perl"},
    {&rDefinition, "R"},
    {&batchDefinition, "Batch File"},
    {&powershellDefinition, "PowerShell"},
    {&haskellDefinition, "Haskell"},
    {&erlangDefinition, "Erlang"},
    {&lispDefinition, "Lisp / Clojure"},
    {&pascalDefinition, "Pascal"},
    {&fortranDefinition, "Fortran"},
    {&tclDefinition, "Tcl"},
    {&vbDefinition, "Visual Basic"},
    {&dDefinition, "D"},
    {&juliaDefinition, "Julia"},
    {&nimDefinition, "Nim"},
    {&latexDefinition, "LaTeX"},
    {&asmDefinition, "Assembly"},
    {&coffeescriptDefinition, "CoffeeScript"},
    {&verilogDefinition, "Verilog"},
    {&vhdlDefinition, "VHDL"},
    {&ocamlDefinition, "OCaml"},
    {&fsharpDefinition, "F#"},
    {&gdscriptDefinition, "GDScript"},
    {&scalaDefinition, "Scala"},
    {&groovyDefinition, "Groovy"},
    {&objcDefinition, "Objective-C"},
    {&protoDefinition, "Protocol Buffers"},
};

bool matches(const QString& value, const std::initializer_list<const char*> candidates) {
    for (const char* candidate : candidates) {
        if (value == QLatin1String(candidate)) {
            return true;
        }
    }
    return false;
}

} // namespace

const SyntaxDefinition* syntaxDefinitionByName(const QString& name) {
    for (const auto& named : namedDefinitions) {
        if (name == QLatin1String(named.definition->name)) {
            return named.definition;
        }
    }
    return nullptr;
}

const std::vector<SyntaxChoice>& syntaxChoices() {
    static const std::vector<SyntaxChoice> choices = [] {
        std::vector<SyntaxChoice> sorted;
        for (const auto& named : namedDefinitions) {
            sorted.push_back({named.definition->name, named.displayName});
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const SyntaxChoice& left, const SyntaxChoice& right) {
                      return QLatin1String(left.displayName)
                                 .compare(QLatin1String(right.displayName),
                                          Qt::CaseInsensitive) < 0;
                  });
        return sorted;
    }();
    return choices;
}

QString syntaxDisplayName(const QString& name) {
    if (name == QStringLiteral("large-file")) {
        return QStringLiteral("Large File");
    }
    for (const auto& named : namedDefinitions) {
        if (name == QLatin1String(named.definition->name)) {
            return QString::fromLatin1(named.displayName);
        }
    }
    return name;
}

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
    if (matches(fileName, {"gemfile", "rakefile", "podfile", "vagrantfile", "brewfile"})) {
        return rubyDefinition;
    }
    if (matches(fileName, {"jenkinsfile"})) {
        return groovyDefinition;
    }
    if (matches(fileName, {".bashrc", ".zshrc", ".bash_profile", ".profile", ".zprofile"})) {
        return shellDefinition;
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
    if (matches(suffix, {"js", "jsx", "mjs", "cjs", "ts", "tsx", "mts", "cts", "astro"})) {
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
    if (matches(suffix, {"xml", "svg", "xsd", "xsl", "xslt", "xaml", "plist", "csproj", "resx"})) {
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
    if (matches(suffix, {"scss", "sass"})) {
        return scssDefinition;
    }
    if (suffix == QStringLiteral("less")) {
        return lessDefinition;
    }
    if (suffix == QStringLiteral("css")) {
        return cssDefinition;
    }
    static const struct {
        std::initializer_list<const char*> suffixes;
        const SyntaxDefinition* definition;
    } suffixDefinitions[] = {
        {{"pl", "pm", "t", "pod"}, &perlDefinition},
        {{"r", "rmd"}, &rDefinition},
        {{"bat", "cmd"}, &batchDefinition},
        {{"ps1", "psm1", "psd1"}, &powershellDefinition},
        {{"hs", "lhs"}, &haskellDefinition},
        {{"erl", "hrl"}, &erlangDefinition},
        {{"lisp", "lsp", "el", "clj", "cljs", "cljc", "edn", "scm", "ss", "rkt"}, &lispDefinition},
        {{"pas", "pp", "dpr", "lpr"}, &pascalDefinition},
        {{"f", "for", "f77", "f90", "f95", "f03", "f08"}, &fortranDefinition},
        {{"tcl", "tk"}, &tclDefinition},
        {{"vb", "vbs", "bas", "cls", "frm"}, &vbDefinition},
        {{"d", "di"}, &dDefinition},
        {{"jl"}, &juliaDefinition},
        {{"nim", "nims", "nimble"}, &nimDefinition},
        {{"tex", "sty", "ltx", "bib"}, &latexDefinition},
        {{"asm", "s", "nasm", "inc"}, &asmDefinition},
        {{"coffee", "litcoffee"}, &coffeescriptDefinition},
        {{"v", "sv", "svh", "vh"}, &verilogDefinition},
        {{"vhd", "vhdl"}, &vhdlDefinition},
        {{"ml", "mli"}, &ocamlDefinition},
        {{"fs", "fsi", "fsx"}, &fsharpDefinition},
        {{"gd"}, &gdscriptDefinition},
        {{"scala", "sc", "sbt"}, &scalaDefinition},
        {{"groovy", "gradle", "gvy"}, &groovyDefinition},
        {{"m", "mm"}, &objcDefinition},
        {{"proto"}, &protoDefinition},
    };
    for (const auto& entry : suffixDefinitions) {
        for (const char* candidate : entry.suffixes) {
            if (suffix == QLatin1String(candidate)) {
                return *entry.definition;
            }
        }
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
