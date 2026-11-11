#include <Windows.h>
#include <commdlg.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")

static HWND gMain = nullptr;
static HWND gEditor = nullptr;
static HWND gOutput = nullptr;
static HFONT gEditorFont = nullptr;
static HFONT gOutputFont = nullptr;

enum { ID_GENERATE = 101, ID_COMPILE = 102, ID_OPEN = 103, ID_SAVE = 104 };

static const wchar_t* TEMPLATE_SOURCE =
L"#include <Windows.h>\r\n"
L"\r\n"
L"int main(){\r\n"
L"    \r\n    if (MessageBoxW(NULL, L\"Continue?\", L\"Visual CodeX Generated\", MB_YESNO | MB_ICONWARNING) != IDYES)\r\n"
L"    {\r\n"
L"        ExitProcess(0);\r\n"
L"    }\r\n"
L"\r\n"
L"    MessageBoxW(NULL, L\"You selected Yes.\", L\"Visual CodeX Generated\", MB_OK | MB_ICONINFORMATION);\r\n"
L"\r\n}";

static void Log ( const std::wstring& s )
{
    if (!gOutput) return;
    int n = GetWindowTextLengthW ( gOutput );
    SendMessageW ( gOutput, EM_SETSEL, n, n );
    std::wstring line = s + L"\r\n";
    SendMessageW ( gOutput, EM_REPLACESEL, FALSE, (LPARAM)line.c_str ( ) );
}

static std::wstring GetEditorText ( )
{
    int n = GetWindowTextLengthW ( gEditor );
    std::wstring s ( (size_t)n + 1, L'\0' );
    if (n) GetWindowTextW ( gEditor, &s[0], n + 1 );
    s.resize ( (size_t)n );
    return s;
}

static bool IsSpace ( wchar_t c ) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

static size_t SkipSpace ( const std::wstring& s, size_t p )
{
    while (p < s.size ( ) && IsSpace ( s[p] )) ++p;
    return p;
}

static bool StartsAt ( const std::wstring& s, size_t p, const wchar_t* token )
{
    size_t n = wcslen ( token );
    return p + n <= s.size ( ) && s.compare ( p, n, token ) == 0;
}

static bool ParseWideLiteral ( const std::wstring& s, size_t& p, std::wstring& out )
{
    p = SkipSpace ( s, p );
    if (p >= s.size ( ) || s[p] != L'L') return false;
    ++p;
    if (p >= s.size ( ) || s[p] != L'"') return false;
    ++p;
    out.clear ( );

    while (p < s.size ( ))
    {
        wchar_t c = s[p++];
        if (c == L'"') return true;
        if (c != L'\\') { out.push_back ( c ); continue; }
        if (p >= s.size ( )) return false;

        wchar_t e = s[p++];
        switch (e)
        {
        case L'n': out.push_back ( L'\n' ); break;
        case L'r': out.push_back ( L'\r' ); break;
        case L't': out.push_back ( L'\t' ); break;
        case L'\\': out.push_back ( L'\\' ); break;
        case L'"': out.push_back ( L'"' ); break;
        case L'0': out.push_back ( L'\0' ); break;
        default: return false;
        }
    }
    return false;
}

static bool SplitArguments ( const std::wstring& s, size_t open, size_t& close, std::vector<std::wstring>& args )
{
    if (open >= s.size ( ) || s[open] != L'(') return false;
    size_t start = open + 1;
    int depth = 1;
    bool inString = false, wide = false, escaped = false;
    args.clear ( );

    for (size_t i = open + 1; i < s.size ( ); ++i)
    {
        wchar_t c = s[i];
        if (inString)
        {
            if (escaped) { escaped = false; continue; }
            if (c == L'\\') { escaped = true; continue; }
            if (c == L'"') inString = false;
            continue;
        }
        if (c == L'"') { inString = true; wide = (i > 0 && s[i - 1] == L'L'); continue; }
        (void)wide;
        if (c == L'(') ++depth;
        else if (c == L')')
        {
            if (--depth == 0)
            {
                args.push_back ( s.substr ( start, i - start ) );
                close = i;
                return true;
            }
        }
        else if (c == L',' && depth == 1)
        {
            args.push_back ( s.substr ( start, i - start ) );
            start = i + 1;
        }
    }
    return false;
}

static std::wstring Trim ( std::wstring s )
{
    size_t a = 0, b = s.size ( );
    while (a < b && IsSpace ( s[a] )) ++a;
    while (b > a && IsSpace ( s[b - 1] )) --b;
    return s.substr ( a, b - a );
}

struct DialogCall
{
    std::wstring text;
    std::wstring title;
    DWORD flags = 0;
    bool conditional = false;
    int expected = 0;
};

static bool ParseFlags ( const std::wstring& raw, DWORD& flags )
{
    std::wstring s = raw;
    s.erase ( std::remove_if ( s.begin ( ), s.end ( ), IsSpace ), s.end ( ) );
    flags = 0;
    size_t p = 0;
    while (p < s.size ( ))
    {
        size_t q = s.find ( L'|', p );
        std::wstring part = s.substr ( p, q == std::wstring::npos ? q : q - p );
        DWORD v = 0;
        if (part == L"MB_OK") v = MB_OK;
        else if (part == L"MB_YESNO") v = MB_YESNO;
        else if (part == L"MB_OKCANCEL") v = MB_OKCANCEL;
        else if (part == L"MB_ICONWARNING") v = MB_ICONWARNING;
        else if (part == L"MB_ICONINFORMATION") v = MB_ICONINFORMATION;
        else if (part == L"MB_ICONERROR") v = MB_ICONERROR;
        else if (part == L"MB_ICONQUESTION") v = MB_ICONQUESTION;
        else return false;
        flags |= v;
        if (q == std::wstring::npos) break;
        p = q + 1;
    }
    return true;
}

static bool FindConditional ( const std::wstring& source, size_t callPos, size_t callEnd, bool& conditional, int& expected )
{
    conditional = false;
    expected = 0;

    size_t ifPos = source.rfind ( L"if", callPos );
    if (ifPos == std::wstring::npos) return true;
    if (ifPos > 0 && (iswalnum ( source[ifPos - 1] ) || source[ifPos - 1] == L'_')) return true;
    size_t p = SkipSpace ( source, ifPos + 2 );
    if (p >= source.size ( ) || source[p] != L'(') return true;
    if (!(p < callPos && callPos < callEnd)) return true;

    int depth = 1;
    size_t condEnd = p + 1;
    for (; condEnd < source.size ( ) && depth; ++condEnd)
    {
        if (source[condEnd] == L'(') ++depth;
        else if (source[condEnd] == L')') --depth;
    }
    if (depth != 0 || callEnd >= condEnd) return true;

    std::wstring tail = source.substr ( callEnd, condEnd - callEnd );
    size_t ne = tail.find ( L"!=" );
    if (ne == std::wstring::npos) return true;
    std::wstring rhs = Trim ( tail.substr ( ne + 2 ) );
    if (rhs.find ( L"IDYES" ) == 0) expected = IDYES;
    else if (rhs.find ( L"IDNO" ) == 0) expected = IDNO;
    else if (rhs.find ( L"IDOK" ) == 0) expected = IDOK;
    else if (rhs.find ( L"IDCANCEL" ) == 0) expected = IDCANCEL;
    else return false;

    size_t body = SkipSpace ( source, condEnd );
    if (body >= source.size ( ) || source[body] != L'{') return false;
    size_t bodyEnd = source.find ( L'}', body + 1 );
    if (bodyEnd == std::wstring::npos) return false;
    std::wstring bodyText = source.substr ( body + 1, bodyEnd - body - 1 );
    if (bodyText.find ( L"ExitProcess" ) == std::wstring::npos) return false;

    conditional = true;
    return true;
}

static bool ParseSource ( const std::wstring& source, std::vector<DialogCall>& calls, std::wstring& error )
{
    calls.clear ( );
    size_t search = 0;

    while ((search = source.find ( L"MessageBoxW", search )) != std::wstring::npos)
    {
        size_t p = search + 11;
        p = SkipSpace ( source, p );
        if (p >= source.size ( ) || source[p] != L'(') { search += 11; continue; }

        size_t close = 0;
        std::vector<std::wstring> args;
        if (!SplitArguments ( source, p, close, args ) || args.size ( ) != 4)
        {
            error = L"MessageBoxW must have exactly four arguments.";
            return false;
        }

        DialogCall call;
        size_t a = 0, b = 0;
        if (!ParseWideLiteral ( args[1], a, call.text ) ||
            !ParseWideLiteral ( args[2], b, call.title ))
        {
            error = L"Arguments 2 and 3 must be wide string literals (L\"...\").";
            return false;
        }
        if (!ParseFlags ( args[3], call.flags ))
        {
            error = L"Unsupported MessageBoxW flag. Supported: MB_OK, MB_YESNO, MB_OKCANCEL and MB_ICONWARNING/INFORMATION/ERROR/QUESTION.";
            return false;
        }

        if (!FindConditional ( source, search, close, call.conditional, call.expected ))
        {
            error = L"Unsupported if condition. Use MessageBoxW(...) != IDYES/IDNO/IDOK/IDCANCEL with ExitProcess(0) in its block.";
            return false;
        }

        calls.push_back ( std::move ( call ) );
        search = close + 1;
    }

    if (calls.empty ( ))
    {
        error = L"No MessageBoxW call found. Check that the editor contains your source code.";
        return false;
    }
    return true;
}

static DWORD AlignUp ( DWORD n, DWORD a ) { return (n + a - 1) & ~(a - 1); }

static void Put16 ( std::vector<BYTE>& b, DWORD p, WORD v )
{
    if ((size_t)p + 2 <= b.size ( )) memcpy ( &b[p], &v, 2 );
}
static void Put32 ( std::vector<BYTE>& b, DWORD p, DWORD v )
{
    if ((size_t)p + 4 <= b.size ( )) memcpy ( &b[p], &v, 4 );
}
static void PutBytes ( std::vector<BYTE>& b, DWORD p, const void* v, size_t n )
{
    if ((size_t)p + n <= b.size ( )) memcpy ( &b[p], v, n );
}
static void PutA ( std::vector<BYTE>& b, DWORD p, const char* s )
{
    PutBytes ( b, p, s, strlen ( s ) + 1 );
}
static void PutW ( std::vector<BYTE>& b, DWORD p, const std::wstring& s )
{
    PutBytes ( b, p, s.c_str ( ), (s.size ( ) + 1) * sizeof ( wchar_t ) );
}

static void Emit8 ( std::vector<BYTE>& c, BYTE v ) { c.push_back ( v ); }
static void Emit32 ( std::vector<BYTE>& c, DWORD v )
{
    BYTE x[4]; memcpy ( x, &v, 4 ); c.insert ( c.end ( ), x, x + 4 );
}
static void EmitPush ( std::vector<BYTE>& c, DWORD v ) { Emit8 ( c, 0x68 ); Emit32 ( c, v ); }
static void EmitCallIAT ( std::vector<BYTE>& c, DWORD va )
{
    Emit8 ( c, 0xFF ); Emit8 ( c, 0x15 ); Emit32 ( c, va );
}

static bool GeneratePE32 ( const wchar_t* path, const std::vector<DialogCall>& calls, std::wstring& error )
{
    const DWORD imageBase = 0x00400000;
    const DWORD sectionRva = 0x1000;
    const DWORD fileRaw = 0x200;

    std::vector<BYTE> code;
    struct Patch { size_t displacement; size_t target; };
    std::vector<Patch> patches;

    // Layout is calculated after the code is emitted. Data addresses are patched later.
    struct CallPatch { size_t titleAt, textAt, messageIatAt; };
    std::vector<CallPatch> callPatches;
    std::vector<size_t> exitIatPatches;

    for (const auto& call : calls)
    {
        CallPatch cp{};
        EmitPush ( code, call.flags );
        EmitPush ( code, 0 ); cp.titleAt = code.size ( ) - 4;
        EmitPush ( code, 0 ); cp.textAt = code.size ( ) - 4;
        Emit8 ( code, 0x6A ); Emit8 ( code, 0x00 ); // hWnd = NULL
        Emit8 ( code, 0xFF ); Emit8 ( code, 0x15 );
        cp.messageIatAt = code.size ( );
        Emit32 ( code, 0 );
        callPatches.push_back ( cp );

        // IAT absolute VA is patched in the call instruction below.
        if (call.conditional)
        {
            Emit8 ( code, 0x3D ); Emit32 ( code, (DWORD)call.expected ); // cmp eax, ID...
            Emit8 ( code, 0x0F ); Emit8 ( code, 0x84 ); // je next statement
            size_t disp = code.size ( );
            Emit32 ( code, 0 );
            EmitPush ( code, 0 );
            Emit8 ( code, 0xFF ); Emit8 ( code, 0x15 );
            exitIatPatches.push_back ( code.size ( ) );
            Emit32 ( code, 0 ); // ExitProcess IAT
            patches.push_back ( { disp, code.size ( ) } );
        }
    }

    EmitPush ( code, 0 );
    Emit8 ( code, 0xFF ); Emit8 ( code, 0x15 );
    exitIatPatches.push_back ( code.size ( ) );
    Emit32 ( code, 0 ); // final ExitProcess

    DWORD codeSize = (DWORD)code.size ( );
    DWORD imports = AlignUp ( codeSize, 4 );
    DWORD iltUser = imports + 60;
    DWORD iatUser = iltUser + 8;
    DWORD iltKernel = iatUser + 8;
    DWORD iatKernel = iltKernel + 8;
    DWORD nameMessage = iatKernel + 8;
    DWORD nameExit = nameMessage + 16;
    DWORD dllUser = nameExit + 16;
    DWORD dllKernel = dllUser + 16;
    DWORD data = AlignUp ( dllKernel + 16, 4 );

    const DWORD userIatVA = imageBase + sectionRva + iatUser;
    const DWORD kernelIatVA = imageBase + sectionRva + iatKernel;

    std::vector<DWORD> textOffsets, titleOffsets;
    DWORD cursor = data;
    for (const auto& call : calls)
    {
        textOffsets.push_back ( cursor );
        cursor += AlignUp ( (DWORD)((call.text.size ( ) + 1) * sizeof ( wchar_t )), 2 );
        titleOffsets.push_back ( cursor );
        cursor += AlignUp ( (DWORD)((call.title.size ( ) + 1) * sizeof ( wchar_t )), 2 );
    }

    DWORD virtualSize = cursor;
    DWORD rawSize = AlignUp ( virtualSize, 0x200 );
    if (rawSize > 16 * 1024 * 1024)
    {
        error = L"Generated program is too large.";
        return false;
    }

    // Patch each MessageBoxW and ExitProcess indirect-call operand.
    for (const auto& cp : callPatches)
        memcpy ( &code[cp.messageIatAt], &userIatVA, 4 );
    for (size_t at : exitIatPatches)
        memcpy ( &code[at], &kernelIatVA, 4 );

    for (const auto& p : patches)
    {
        int32_t rel = (int32_t)(p.target - (p.displacement + 4));
        memcpy ( &code[p.displacement], &rel, 4 );
    }

    for (size_t i = 0; i < calls.size ( ); ++i)
    {
        DWORD titleVA = imageBase + sectionRva + titleOffsets[i];
        DWORD textVA = imageBase + sectionRva + textOffsets[i];
        memcpy ( &code[callPatches[i].titleAt], &titleVA, 4 );
        memcpy ( &code[callPatches[i].textAt], &textVA, 4 );
    }

    std::vector<BYTE> file ( fileRaw + rawSize, 0 );
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    PutBytes ( file, 0, &dos, sizeof ( dos ) );
    Put32 ( file, 0x80, IMAGE_NT_SIGNATURE );

    IMAGE_FILE_HEADER fh{};
    fh.Machine = IMAGE_FILE_MACHINE_I386;
    fh.NumberOfSections = 1;
    fh.SizeOfOptionalHeader = sizeof ( IMAGE_OPTIONAL_HEADER32 );
    fh.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE;
    PutBytes ( file, 0x84, &fh, sizeof ( fh ) );

    IMAGE_OPTIONAL_HEADER32 oh{};
    oh.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    oh.MajorLinkerVersion = 14;
    oh.SizeOfCode = rawSize;
    oh.AddressOfEntryPoint = sectionRva;
    oh.BaseOfCode = sectionRva;
    oh.ImageBase = imageBase;
    oh.SectionAlignment = 0x1000;
    oh.FileAlignment = 0x200;
    oh.MajorOperatingSystemVersion = 5;
    oh.MajorSubsystemVersion = 5;
    oh.SizeOfImage = AlignUp ( sectionRva + virtualSize, 0x1000 );
    oh.SizeOfHeaders = fileRaw;
    oh.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
    oh.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
    oh.SizeOfStackReserve = 0x100000;
    oh.SizeOfStackCommit = 0x1000;
    oh.SizeOfHeapReserve = 0x100000;
    oh.SizeOfHeapCommit = 0x1000;
    oh.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = sectionRva + imports;
    oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = 60;
    PutBytes ( file, 0x98, &oh, sizeof ( oh ) );

    IMAGE_SECTION_HEADER sh{};
    memcpy ( sh.Name, ".text", 5 );
    sh.Misc.VirtualSize = virtualSize;
    sh.VirtualAddress = sectionRva;
    sh.SizeOfRawData = rawSize;
    sh.PointerToRawData = fileRaw;
    sh.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_CNT_INITIALIZED_DATA |
        IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
    PutBytes ( file, 0x98 + sizeof ( oh ), &sh, sizeof ( sh ) );

    DWORD base = fileRaw;
    PutBytes ( file, base, code.data ( ), code.size ( ) );

    // Import descriptors: USER32 and KERNEL32, followed by null descriptor.
    Put32 ( file, base + imports + 0, sectionRva + iltUser );
    Put32 ( file, base + imports + 12, sectionRva + dllUser );
    Put32 ( file, base + imports + 16, sectionRva + iatUser );
    Put32 ( file, base + imports + 20, sectionRva + iltKernel );
    Put32 ( file, base + imports + 32, sectionRva + dllKernel );
    Put32 ( file, base + imports + 36, sectionRva + iatKernel );

    Put32 ( file, base + iltUser, sectionRva + nameMessage );
    Put32 ( file, base + iatUser, sectionRva + nameMessage );
    Put32 ( file, base + iltKernel, sectionRva + nameExit );
    Put32 ( file, base + iatKernel, sectionRva + nameExit );
    Put16 ( file, base + nameMessage, 0 );
    PutA ( file, base + nameMessage + 2, "MessageBoxW" );
    Put16 ( file, base + nameExit, 0 );
    PutA ( file, base + nameExit + 2, "ExitProcess" );
    PutA ( file, base + dllUser, "USER32.dll" );
    PutA ( file, base + dllKernel, "KERNEL32.dll" );

    for (size_t i = 0; i < calls.size ( ); ++i)
    {
        PutW ( file, base + textOffsets[i], calls[i].text );
        PutW ( file, base + titleOffsets[i], calls[i].title );
    }

    std::ofstream out ( path, std::ios::binary | std::ios::trunc );
    if (!out)
    {
        error = L"Could not create the output file. Check folder permissions.";
        return false;
    }
    out.write ( (const char*)file.data ( ), (std::streamsize)file.size ( ) );
    out.close ( );
    if (!out)
    {
        error = L"An error occurred while writing the executable.";
        return false;
    }
    return true;
}

static void CompileSource ( )
{
    std::wstring source = GetEditorText ( );
    Log ( L"----------------------------------------" );
    Log ( L"MiniCompiler: parsing source..." );
    std::vector<DialogCall> calls;
    std::wstring error;
    if (!ParseSource ( source, calls, error ))
    {
        Log ( L"Compile error: " + error );
        return;
    }

    // Ask where to save the generated application and what to name it.
    wchar_t outputPath[MAX_PATH] = L"MyProgram.exe";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ( ofn );
    ofn.hwndOwner = gMain;
    ofn.lpstrFilter = L"Windows Executable (*.exe)\\0*.exe\\0All Files (*.*)\\0*.*\\0";
    ofn.lpstrFile = outputPath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"exe";
    ofn.lpstrTitle = L"Choose MiniCompiler output name";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

    if (!GetSaveFileNameW ( &ofn ))
    {
        DWORD dlgError = CommDlgExtendedError ( );
        if (dlgError != 0)
            Log ( L"Save dialog error: " + std::to_wstring ( dlgError ) );
        else
            Log ( L"Compilation cancelled." );
        return;
    }

    // Ensure the generated application has an .exe extension.
    std::wstring chosenPath ( outputPath );
    size_t slash = chosenPath.find_last_of ( L"\\/" );
    size_t dot = chosenPath.find_last_of ( L'.' );
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        chosenPath += L".exe";

    Log ( L"Parsed " + std::to_wstring ( calls.size ( ) ) + L" MessageBoxW call(s)." );
    Log ( L"Generating standalone x86 PE32 executable..." );
    if (!GeneratePE32 ( chosenPath.c_str ( ), calls, error ))
    {
        Log ( L"Generation failed: " + error );
        return;
    }

    Log ( L"Success: " + chosenPath );
    Log ( L"Target: x86 / PE32" );
    Log ( L"Supported subset: MessageBoxW and simple != ID... ExitProcess blocks." );
}

static void GenerateTemplate ( )
{
    SetWindowTextW ( gEditor, TEMPLATE_SOURCE );
    Log ( L"Inserted supported-language example." );
}

static void OpenSource ( )
{
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ( ofn );
    ofn.hwndOwner = gMain;
    ofn.lpstrFilter = L"C/C++ Source Files\0*.cpp;*.c;*.h\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW ( &ofn )) return;

    std::wifstream in ( path );
    if (!in) { Log ( L"Could not open source file." ); return; }
    std::wstringstream buffer;
    buffer << in.rdbuf ( );
    SetWindowTextW ( gEditor, buffer.str ( ).c_str ( ) );
    Log ( L"Source opened." );
}

static void SaveSource ( )
{
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ( ofn );
    ofn.hwndOwner = gMain;
    ofn.lpstrFilter = L"C++ Source Files\0*.cpp\0All Files\0*.*\0";
    ofn.lpstrDefExt = L"cpp";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW ( &ofn )) return;

    std::wofstream out ( path );
    if (!out) { Log ( L"Could not save source file." ); return; }
    out << GetEditorText ( );
    Log ( out.good ( ) ? L"Source saved." : L"Error saving source." );
}

static HWND CreateControl ( HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id )
{
    return CreateWindowExW ( 0, cls, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW ( nullptr ), nullptr );
}

static void Layout ( HWND hwnd )
{
    RECT r{};
    GetClientRect ( hwnd, &r );
    int w = r.right, h = r.bottom;
    MoveWindow ( gEditor, 10, 48, max ( 100, w - 20 ), max ( 100, h - 180 ), TRUE );
    MoveWindow ( gOutput, 10, h - 122, max ( 100, w - 20 ), 110, TRUE );
    MoveWindow ( GetDlgItem ( hwnd, ID_GENERATE ), 10, 10, 100, 30, TRUE );
    MoveWindow ( GetDlgItem ( hwnd, ID_COMPILE ), 120, 10, 100, 30, TRUE );
    MoveWindow ( GetDlgItem ( hwnd, ID_OPEN ), 230, 10, 80, 30, TRUE );
    MoveWindow ( GetDlgItem ( hwnd, ID_SAVE ), 320, 10, 80, 30, TRUE );
}

static LRESULT CALLBACK WindowProc ( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_CREATE:
        gMain = hwnd;
        CreateControl ( hwnd, L"BUTTON", L"Generate", BS_PUSHBUTTON, ID_GENERATE );
        CreateControl ( hwnd, L"BUTTON", L"Compile", BS_PUSHBUTTON, ID_COMPILE );
        CreateControl ( hwnd, L"BUTTON", L"Open", BS_PUSHBUTTON, ID_OPEN );
        CreateControl ( hwnd, L"BUTTON", L"Save", BS_PUSHBUTTON, ID_SAVE );

        gEditor = CreateWindowExW ( WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
            ES_WANTRETURN | WS_VSCROLL | WS_HSCROLL,
            10, 48, 900, 450, hwnd, (HMENU)200, GetModuleHandleW ( nullptr ), nullptr );

        gOutput = CreateWindowExW ( WS_EX_CLIENTEDGE, L"EDIT", L"MiniCompiler ready.\r\n",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            10, 500, 900, 110, hwnd, (HMENU)201, GetModuleHandleW ( nullptr ), nullptr );

        gEditorFont = CreateFontW ( 18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas" );
        gOutputFont = CreateFontW ( 14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas" );
        SendMessageW ( gEditor, WM_SETFONT, (WPARAM)gEditorFont, TRUE );
        SendMessageW ( gOutput, WM_SETFONT, (WPARAM)gOutputFont, TRUE );
        SendMessageW ( gEditor, EM_LIMITTEXT, 1024 * 1024, 0 );
        return 0;

    case WM_SIZE:
        Layout ( hwnd );
        return 0;

    case WM_COMMAND:
        switch (LOWORD ( wp ))
        {
        case ID_GENERATE: GenerateTemplate ( ); return 0;
        case ID_COMPILE: CompileSource ( ); return 0;
        case ID_OPEN: OpenSource ( ); return 0;
        case ID_SAVE: SaveSource ( ); return 0;
        }
        break;

    case WM_DESTROY:
        if (gEditorFont) DeleteObject ( gEditorFont );
        if (gOutputFont) DeleteObject ( gOutputFont );
        PostQuitMessage ( 0 );
        return 0;
    }
    return DefWindowProcW ( hwnd, msg, wp, lp );
}

int WINAPI wWinMain ( HINSTANCE instance, HINSTANCE, PWSTR, int show ) {

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW ( nullptr, IDC_ARROW );
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MiniCompilerWindowClass";
    if (!RegisterClassW ( &wc )) return 1;

    HWND hwnd = CreateWindowExW ( 0, wc.lpszClassName,
        L"Visual CodeX",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1050, 750,
        nullptr, nullptr, instance, nullptr );
    if (!hwnd) return 1;
    ShowWindow ( hwnd, show );
    UpdateWindow ( hwnd );

    MSG msg{};
    while (GetMessageW ( &msg, nullptr, 0, 0 ) > 0)
    {
        TranslateMessage ( &msg );
        DispatchMessageW ( &msg );
    }
    return (int)msg.wParam;
}
