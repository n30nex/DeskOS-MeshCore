from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"static\s+(?:bool|void)\s+" + name + r"\([^)]*\)\s*\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def test_map_labels_respect_current_controls_and_open_their_node(tmp_path):
    source = (ROOT / "main/ui/ui_map.c").read_text()
    structs = source.split("typedef struct {\n    int16_t x1;", 1)[1].split("static uint32_t s_viewport_generation", 1)[0]
    program = tmp_path / "map_label.c"
    program.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define D1L_NODE_FINGERPRINT_LEN 17
#define MAP_VIEWPORT_WIDTH 478
#define MAP_VIEWPORT_HEIGHT 430
#define MAP_MARKER_HIT_RADIUS 22
typedef struct {int16_t x,y;} lv_point_t;
typedef struct {int16_t x1,y1,x2,y2;} lv_area_t;
typedef int lv_indev_t;
''' + "typedef struct {\n    int16_t x1;" + structs + r'''
static int viewport;
static void *s_viewport_obj=&viewport;
static size_t s_viewport_marker_count=2, s_viewport_marker_label_count;
static uint32_t s_viewport_marker_frame_generation=1;
static d1l_ui_map_marker_hit_t s_viewport_marker_hits[2];
static d1l_ui_map_marker_rect_t s_viewport_marker_bounds[8];
static lv_point_t tap;
static int lease_calls, open_count;
static bool stale_after_first;
static char opened[17];
static bool map_viewport_marker_lease_valid(uint32_t generation) {
    return generation==1 && !(stale_after_first && ++lease_calls>1);
}
static lv_indev_t *lv_indev_get_act(void) {return &viewport;}
static void lv_indev_get_point(lv_indev_t *input, lv_point_t *point) {(void)input; *point=tap;}
static void lv_obj_get_coords(void *object, lv_area_t *area) {(void)object; memset(area,0,sizeof(*area));}
static void open_node(const char *fingerprint) {strcpy(opened,fingerprint); open_count++;}
static void (*s_viewport_open_node_detail)(const char *)=open_node;
''' + "\n".join(function(source, name) for name in [
        "map_copy_bounded_text", "map_marker_rects_intersect", "map_marker_placement_allowed", "map_viewport_try_marker_tap"
    ]) + r'''
int main(void) {
    char text[6];
    map_copy_bounded_text(text,5,"abc\xc3\xa9");
    assert(strcmp(text,"abc")==0);
    map_copy_bounded_text(text,sizeof(text),"abc\xc3\xa9");
    assert(strcmp(text,"abc\xc3\xa9")==0);
    d1l_ui_map_marker_rect_t middle={10,280,30,305}, bottom={10,380,30,410};
    assert(map_marker_placement_allowed(&middle));
    assert(!map_marker_placement_allowed(&bottom));
    s_viewport_marker_hits[0]=(d1l_ui_map_marker_hit_t){
      .screen_x=100,.screen_y=100,.label_visible=true,.label_bounds={44,110,156,148},
      .fingerprint="0123456789abcdef"};
    s_viewport_marker_hits[1]=(d1l_ui_map_marker_hit_t){
      .screen_x=100,.screen_y=140,.fingerprint="fedcba9876543210"};
    tap=(lv_point_t){101,141}; /* The label is beyond the original dot-only radius. */
    assert(map_viewport_try_marker_tap());
    assert(strcmp(opened,"0123456789abcdef")==0);
    stale_after_first=true; lease_calls=0;
    assert(!map_viewport_try_marker_tap());
    assert(open_count==1);
    return 0;
}
''')
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for map interaction tests"
    binary = tmp_path / "map_label"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(program), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
