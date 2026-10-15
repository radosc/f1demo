// Entry point: game launcher and command line tools.
#include "core/base.h"
#include "core/fs.h"
#include "convert/convert.h"
#include "core/png.h"
#include "gfx/upscale.h"

#include <windows.h>
#include <shellapi.h>

namespace fo {
int runGame(const std::vector<std::string>& args); // app.cpp
}

using namespace fo;

static bool g_console = false;

static void attachConsole()
{
    if (g_console) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        g_console = true;
    }
}

static std::vector<std::string> commandLineArgs()
{
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) args.push_back(wideToUtf8(argv[i]));
    LocalFree(argv);
    return args;
}

static int runConvert(const std::vector<std::string>& args)
{
    attachConsole();
    ConvertOptions opt;
    opt.outDir = pathJoin(exeDir(), "../data");
    for (size_t i = 1; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--scale" && i + 1 < args.size()) opt.hdScale = atoi(args[++i].c_str());
        else if (a == "--out" && i + 1 < args.size()) opt.outDir = args[++i];
        else if (a == "--no-art") opt.art = false;
        else if (a == "--only-art") { opt.text = opt.protos = opt.maps = opt.scripts = opt.sound = opt.misc = false; }
        else if (a == "--no-hd") opt.hdScale = 0;
        else if (a == "--hd-style" && i + 1 < args.size()) opt.hdStyle = args[++i] == "pixel" ? 0 : 1;
        else if (opt.datPath.empty()) opt.datPath = a;
    }
    if (opt.datPath.empty()) {
        printf("usage: FalloutDemoHD --convert <FALLDEMO.DAT> [--out dir] [--scale 2|4] [--hd-style smooth|pixel] [--no-hd] [--no-art] [--only-art]\n");
        return 1;
    }
    double t0 = timeSeconds();
    opt.onProgress = [](const std::string& stage, float p) {
        printf("\r%-10s %5.1f%%", stage.c_str(), p * 100.0f);
        fflush(stdout);
    };
    bool ok = convertGameData(opt);
    printf("\n%s in %.1f s -> %s\n", ok ? "Conversion finished" : "Conversion FAILED", timeSeconds() - t0, opt.outDir.c_str());
    return ok ? 0 : 2;
}

// --hd-preview <indexed sheet.png> <out prefix>: writes <prefix>_smooth.png and <prefix>_pixel.png (4x)
static int runHdPreview(const std::vector<std::string>& args)
{
    attachConsole();
    if (args.size() < 3) {
        printf("usage: FalloutDemoHD --hd-preview <sheet.png> <out prefix>\n");
        return 1;
    }
    Bytes data;
    Image img;
    if (!fileRead(args[1], data) || !pngDecode(data.data(), data.size(), img) || !img.indexed) {
        printf("cannot read indexed PNG %s\n", args[1].c_str());
        return 2;
    }
    std::vector<u32> pal = img.palette;
    pal.resize(256, 0);
    double t0 = timeSeconds();
    Image smooth = upscaleSmooth(indexedToRGBA(img, pal.data()), 4);
    bleedAlpha(smooth);
    double t1 = timeSeconds();
    int luma[256];
    buildLumaTable(pal.data(), luma);
    Image pixel = upscaleIndexed(img, 4, luma);
    Bytes a = pngEncode(smooth, 6), b = pngEncode(pixel, 6);
    fileWrite(args[2] + "_smooth.png", a.data(), a.size());
    fileWrite(args[2] + "_pixel.png", b.data(), b.size());
    printf("smooth %.0f ms -> %s_smooth.png / _pixel.png\n", (t1 - t0) * 1000.0, args[2].c_str());
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    std::vector<std::string> args = commandLineArgs();
    std::string logPath = pathJoin(exeDir(), "fo1hd.log");
    bool convert = !args.empty() && args[0] == "--convert";
    if (convert) logPath = pathJoin(exeDir(), "convert.log");
    logInit(logPath);
    LOGI("Fallout Demo HD starting");
    g_rng.seed((u64)GetTickCount64() * 2654435761ull);
    for (size_t i = 0; i + 1 < args.size(); i++)
        if (args[i] == "--seed") g_rng.seed(strtoull(args[i + 1].c_str(), nullptr, 10)); // reproducible scripted runs
    if (convert) return runConvert(args);
    if (!args.empty() && args[0] == "--hd-preview") return runHdPreview(args);
    return runGame(args);
}
