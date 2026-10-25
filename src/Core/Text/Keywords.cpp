#include "Text/Keywords.h"

#include <ILexer.h>

namespace fastlook::core
{
    namespace
    {
        // "cpp" is shared by C, C++, C#, Java, JavaScript, TypeScript, Go, Kotlin, Swift and Dart: one union list.
        constexpr char kCpp[] =
            "alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class compl concept "
            "const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do double "
            "dynamic_cast else enum explicit export extern false final float for friend goto if inline int long mutable namespace "
            "new noexcept not not_eq nullptr operator or or_eq override private protected public register reinterpret_cast requires "
            "return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try "
            "typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq "
            "abstract as async await base byte checked decimal delegate event fixed foreach implicit in interface internal "
            "is lock null object out params readonly ref sbyte sealed stackalloc string uint ulong unchecked unsafe ushort var "
            "boolean extends finally implements import instanceof native package strictfp super synchronized throws transient "
            "arguments debugger eval function let of typeof undefined with yield "
            "any declare get infer keyof module never set symbol type unknown "
            "chan defer fallthrough func go map range select "
            "fun val when crossinline noinline reified suspend tailrec vararg "
            "associatedtype deinit extension fileprivate guard init inout open protocol rethrows subscript Self "
            "mixin on part late required show hide";
        constexpr char kPython[] =
            "False None True and as assert async await break case class continue def del elif else except finally for from global "
            "if import in is lambda match nonlocal not or pass raise return try while with yield";
        constexpr char kRust[] =
            "Self as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut "
            "pub ref return self static struct super trait true type unsafe use where while "
            "bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize";
        // The SQL lexer lower-cases a word before looking it up.
        constexpr char kSql[] =
            "add all alter and any as asc backup between by case check column constraint create database default delete desc distinct "
            "drop exec exists foreign from full group having in index inner insert into is join key left like limit not null on or "
            "order outer primary procedure replace right rownum select set table top truncate union unique update values view where "
            "begin commit declare else end if rollback then transaction with";
        constexpr char kBash[] =
            "alias bg bind break builtin case cd command compgen complete continue declare dirs disown do done echo elif else enable "
            "esac eval exec exit export false fc fg fi for function getopts hash help history if in jobs kill let local logout "
            "popd printf pushd pwd read readonly return select set shift shopt source suspend test then time times trap true type "
            "typeset ulimit umask unalias unset until wait while";
        // The PowerShell lexer lower-cases a word before looking it up.
        constexpr char kPowerShell[] =
            "begin break catch class continue data do dynamicparam else elseif end enum exit filter finally for foreach from "
            "function hidden if in param process return static switch throw trap try until using var while workflow";
        constexpr char kBatch[] =
            "assoc break call cd chcp chdir cls color copy date del dir do echo else endlocal erase errorlevel exist exit for ftype "
            "goto if in md mkdir mklink move not path pause popd prompt pushd rd rem ren rename rmdir set setlocal shift start time "
            "title type ver verify vol";
        constexpr char kLua[] =
            "and break do else elseif end false for function goto if in local nil not or repeat return then true until while";
        constexpr char kRuby[] =
            "BEGIN END __ENCODING__ __FILE__ __LINE__ alias and begin break case class def defined? do else elsif end ensure false "
            "for if in module next nil not or redo rescue retry return self super then true undef unless until when while yield";
        constexpr char kPerl[] =
            "NULL __FILE__ __LINE__ __PACKAGE__ __DATA__ __END__ AUTOLOAD BEGIN CORE DESTROY END EQ GE GT INIT LE LT NE CHECK UNITCHECK "
            "abs accept alarm and atan2 bind binmode bless caller chdir chmod chomp chop chown chr chroot close closedir cmp connect "
            "continue cos crypt defined delete die do dump each else elsif eof eval exec exists exit exp fcntl fileno flock for "
            "foreach fork format formline getc glob gmtime goto grep hex if index int ioctl join keys kill last lc lcfirst length "
            "link listen local localtime log lstat map mkdir my next no not oct open opendir or ord our pack package pipe pop pos "
            "print printf prototype push quotemeta rand read readdir readline redo ref rename require reset return reverse rewinddir "
            "rindex rmdir say scalar seek select shift sin sleep sort splice split sprintf sqrt srand stat state study sub substr "
            "symlink syscall system tell time tr truncate uc ucfirst umask undef unless unlink unpack unshift untie until use "
            "utime values vec wait waitpid wantarray warn while write xor y";
        constexpr char kPhp[] =
            "__halt_compiler abstract and array as break callable case catch class clone const continue declare default die do echo "
            "else elseif empty enddeclare endfor endforeach endif endswitch endwhile enum eval exit extends false final finally fn "
            "for foreach function global goto if implements include include_once instanceof insteadof interface isset list match "
            "namespace new null or print private protected public readonly require require_once return static switch throw trait "
            "true try unset use var while xor yield";
        constexpr char kJson[] = "false null true";

        char const* ListFor(std::string_view name)
        {
            if (name == "cpp") return kCpp;
            if (name == "python") return kPython;
            if (name == "rust") return kRust;
            if (name == "sql") return kSql;
            if (name == "bash") return kBash;
            if (name == "powershell") return kPowerShell;
            if (name == "batch") return kBatch;
            if (name == "lua") return kLua;
            if (name == "ruby") return kRuby;
            if (name == "perl") return kPerl;
            if (name == "phpscript") return kPhp;
            if (name == "json") return kJson;
            return nullptr;
        }
    }

    void ApplyKeywords(Scintilla::ILexer5* lexer, std::string_view lexerName)
    {
        if (!lexer) return;
        if (char const* const list = ListFor(lexerName))
        {
            lexer->WordListSet(0, list);
        }
    }
}
