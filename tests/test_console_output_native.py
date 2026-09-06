import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_console_records_stay_whole_between_async_logs_and_recover_after_overflow(tmp_path):
    source = tmp_path / "output.c"
    console_source = (ROOT / "main/comms/usb_console.c").read_text(encoding="utf-8")
    helper = console_source.split("static void print_json_string", 1)[1].split("\n}\n", 1)[0]
    helper = "static void print_json_string" + helper + "\n}\n"
    prelude = '#include <stdio.h>\n#include <stdint.h>\n#include "comms/console_output.h"\n#define printf d1l_console_printf\n#define putchar d1l_console_putchar\n'
    prelude += helper + '\n#undef printf\n#undef putchar\n'
    source.write_text(prelude + r'''
#include <assert.h>
#include <stdio.h>
#include "comms/console_output.h"
int main(void) {
    d1l_console_printf("d1l> \n");
    d1l_console_printf("{\"schema\":1,\"ok\":true,\"cmd\":\"observer status\",\"topic\":");
    printf("I (195044) wifi: init -> auth\n");
    d1l_console_putchar('"');
    const char *text="meshcore/TEST/public";
    for (const char *p=text; *p; ++p) d1l_console_putchar(*p);
    printf("I (195052) wifi: auth -> assoc\n");
    d1l_console_printf("\",\"connected\":%s}\n", "true");
    d1l_console_printf("{\"cmd\":\"large\",\"body\":\"");
    for (int i=0;i<5;++i) (void)d1l_console_printf("%65536d",i);
    d1l_console_printf("\"}\n");
    d1l_console_printf("{\"cmd\":\"after\",\"value\":%u}", 42U);
    d1l_console_putchar('\n');
    d1l_console_printf("{\"cmd\":\"escaped\",\"value\":");
    print_json_string("quote \" slash \\ newline \n tab \t");
    d1l_console_printf("}\n");
    d1l_console_printf("{\"cmd\":\"long-valid\",\"value\":\"%70000d\"}\n", 1);
    d1l_console_printf("d1l> ");
    fflush(stdout);
    return 0;
}
''', encoding="utf-8")
    executable = tmp_path / "output"
    subprocess.run([shutil.which("gcc") or "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "main"), str(ROOT / "main/comms/console_output.c"),
                    str(source), "-o", str(executable)], check=True, capture_output=True, text=True)
    output = subprocess.check_output([str(executable)], text=True)
    lines = output.splitlines()
    assert lines[1:3] == ["I (195044) wifi: init -> auth", "I (195052) wifi: auth -> assoc"]
    rows = [json.loads(line) for line in lines if line.startswith("{")]
    assert rows[0] == {"schema": 1, "ok": True, "cmd": "observer status", "topic": "meshcore/TEST/public", "connected": True}
    assert rows[1]["code"] == "OUTPUT_TOO_LARGE" and rows[1]["ok"] is False
    assert rows[2] == {"cmd": "after", "value": 42}
    assert rows[3] == {"cmd": "escaped", "value": 'quote " slash \\ newline \n tab \t'}
    assert len(rows[4]["value"]) == 70000
    assert lines[-1] == "d1l> "
    console = (ROOT / "main/comms/usb_console.c").read_text(encoding="utf-8")
    assert "#define printf d1l_console_printf" in console
    assert "#define putchar d1l_console_putchar" in console
