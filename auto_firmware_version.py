import subprocess

Import("env")


def get_firmware_specifier_build_flag():
    ret = subprocess.run(["git", "describe"], stdout=subprocess.PIPE, text=True)
    build_version = ret.stdout.strip().replace("Release", "")
    print("Firmware Revision: " + build_version)
    return '-D AUTO_VERSION=\\"' + build_version + '\\"'


env_name = env.subst("$PIOENV")
board_cfg = env.BoardConfig()
mcu = board_cfg.get("build.mcu", "").lower() if board_cfg else ""
existing_flags_str = " ".join(str(x) for x in env.get("BUILD_FLAGS", []))

extra_flags = [
    get_firmware_specifier_build_flag(),
    f'-D PIOENV_NAME=\\"{env_name}\\"',
]

# Dedicate Core 1 to the hardware SHA miner on dual-core targets and enable
# each target's pipelined HW loop unless explicitly overridden by the env.
if mcu in ("esp32", "esp32s3"):
    if "ARDUINO_RUNNING_CORE" not in existing_flags_str:
        extra_flags.extend(["-U ARDUINO_RUNNING_CORE", "-D ARDUINO_RUNNING_CORE=0"])
    if "ARDUINO_EVENT_RUNNING_CORE" not in existing_flags_str:
        extra_flags.extend(["-U ARDUINO_EVENT_RUNNING_CORE", "-D ARDUINO_EVENT_RUNNING_CORE=0"])

if mcu == "esp32" and "PIPELINED_ASM_MINING" not in existing_flags_str and "FORCE_SW_MINING" not in existing_flags_str:
    extra_flags.append("-D PIPELINED_ASM_MINING=1")
elif mcu == "esp32s3" and "PIPELINED_S3_MINING" not in existing_flags_str and "FORCE_SW_MINING" not in existing_flags_str:
    extra_flags.append("-D PIPELINED_S3_MINING=1")

env.Append(BUILD_FLAGS=extra_flags)

# Compile cold code (WebServer, WiFiManager, ArduinoJson, TFT_eSPI, OpenFontRender,
# web UI, CLI, OTA, storage) with -Os to save ~100 KB of Flash, while compiling
# only the mining hot-path translation units with -O3 (+ -funroll-loops).
HOT_UNROLL_FILES = {
    "mining.cpp",
    "pipelined_hw_sha_classic.cpp",
    "sha_bench.cpp",
    "sha_bench_s3.cpp",
    "sha_bench_var.cpp",
    "sha_bench_x.cpp",
}
HOT_O3_FILES = {
    "nerdSHA256plus.cpp",
}


def selective_opt_middleware(cenv, node):
    path = node.get_path().replace("\\", "/")
    base = path.rsplit("/", 1)[-1]
    if base in HOT_UNROLL_FILES or base in HOT_O3_FILES:
        e = cenv.Clone()
        for var in ("CCFLAGS", "CXXFLAGS", "CFLAGS"):
            if var in e:
                e[var] = [f for f in e[var] if f not in ("-Os", "-O2", "-O1")]
        opt = ["-O3", "-funroll-loops"] if base in HOT_UNROLL_FILES else ["-O3"]
        e.Append(CCFLAGS=opt)
        return e.Object(node)
    return node


env.AddBuildMiddleware(selective_opt_middleware, "*")
