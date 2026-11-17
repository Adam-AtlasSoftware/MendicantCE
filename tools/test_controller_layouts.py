"""Controller layouts (#1143, docs/controller_remapping.md, "Controller layouts"): the production C of
port/linux/src/controller_layouts.c run on files in a folder of the test's: a layout written and read back the
same, kept by its controller's GUID; a file copied into the folder taken as its controller's; a malformed or
hostile file refused whole with its line, nothing of it used, the folder's other layouts kept; the legend black
or white on its fill; the prompts drawing a described extra button's symbol and naming it, and a controller with
no layout (every player who never opens the page) unchanged. Each check has a negative control: the production
code with a fault, which it must catch. The screen's fit: tools/harness/tests/test_controller_layout_screen.py."""
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest

from harness import ABI_FLAGS

ROOT = Path(__file__).resolve().parents[1]
GUID = "030000005e040000e002000000007801"
OTHER = "0300000000f00000f100000000000000"


def source(path, text=None):
    text = text if text is not None else (ROOT / path).read_text()
    return '\n'.join(line for line in text.splitlines() if not line.startswith('#include')) + '\n'


def run_c(tmp_path, code, expect_ok=True):
    compiler = shutil.which('clang')
    if not compiler:
        pytest.skip('clang required')
    folder = tmp_path / 'data'
    (folder / 'controller_layouts').mkdir(parents=True, exist_ok=True)
    c = tmp_path / 'layouts.c'
    binary = tmp_path / 'layouts.exe'
    c.write_text(code)
    target = ['--target=i686-pc-windows-msvc'] if sys.platform == 'win32' else ABI_FLAGS
    result = subprocess.run([compiler, *target, '-std=gnu89', '-O1', '-Wall', '-Wno-unused-function', str(c), '-o',
                             str(binary)], capture_output=True, text=True)
    if result.returncode and 'm32' in ' '.join(target) and 'cannot find' in result.stderr:
        pytest.skip('a 32-bit C library required (gcc-multilib)')
    assert result.returncode == 0, result.stderr
    run = subprocess.run([str(binary), str(folder)], capture_output=True, text=True, timeout=20)
    if expect_ok:
        assert run.returncode == 0, run.stdout + run.stderr
    return run


# the platform the file uses (posix.h's, platform.h's), as the real POSIX calls
PLATFORM = r'''
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static char const *data_root;
static char const *platform_data_root(void){return data_root;}
static char logged[4096];
static void platform_log(char const *format,...){va_list a;size_t n=strlen(logged);va_start(a,format);
 vsnprintf(logged+n,sizeof(logged)-n,format,a);va_end(a);strncat(logged,"\n",sizeof(logged)-strlen(logged)-1);}
typedef unsigned long posix_ulong;
enum {_posix_file_is_directory=1};
struct posix_file_information {posix_ulong flags,size_low,size_high,modification_seconds;};
static int posix_stat(char const *path,struct posix_file_information *i){struct stat s;if(stat(path,&s))return -1;
 memset(i,0,sizeof(*i));i->flags=S_ISDIR(s.st_mode)?1:0;i->size_low=(posix_ulong)s.st_size;return 0;}
static int posix_make_directory(char const *path){return mkdir(path,0755);}
static void *posix_directory_open(char const *path){return opendir(path);}
static int posix_directory_next(void *d,char *name,posix_ulong size){struct dirent *e;
 while((e=readdir((DIR *)d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
  snprintf(name,size,"%s",e->d_name);return 1;}return 0;}
static void posix_directory_close(void *d){closedir((DIR *)d);}
'''
HEADERS = (source('port/linux/include/controller_buttons.h') + source('port/linux/include/controller_remap.h') +
           source('port/linux/include/controller_layouts.h'))
CONFIG = r'''
static unsigned long config_changes(void){return 0;}
static char const *config_string(char const *n){(void)n;return "";}
static int config_player_has(int l,char const *n){(void)l;(void)n;return 0;}
static char const *config_player_string(int l,char const *n){(void)l;(void)n;return "";}
static int config_pinned_by_environment(char const *n){(void)n;return 0;}
'''
HELPERS = r'''
static char path_buffer[1024];
static char const *at(char const *name){snprintf(path_buffer,sizeof(path_buffer),"%s/controller_layouts/%s",
 data_root,name);return path_buffer;}
static void put(char const *name,char const *text){FILE *f=fopen(at(name),"wb");assert(f);fputs(text,f);fclose(f);}
static int exists(char const *name){FILE *f=fopen(at(name),"rb");if(f)fclose(f);return f!=NULL;}
static struct controller_layout sample(char const *guid){
 struct controller_layout l;memset(&l,0,sizeof(l));snprintf(l.guid,sizeof(l.guid),"%s",guid);
 snprintf(l.name,sizeof(l.name),"Fight stick");
 controller_layout_describe(&l.controls[0],"P3");
 controller_layout_describe(&l.controls[3],"RIGHT BACK 2");
 l.controls[3].shape=CONTROLLER_LAYOUT_SQUARE;l.controls[3].fill=0x3070D0;
 controller_layout_describe(&l.controls[10],"MISC 6");snprintf(l.controls[10].name,21,"Turbo");return l;}
'''


def program(body, layouts=None, remap=None):
    return (PLATFORM + HEADERS + CONFIG + source('port/linux/src/controller_remap.c', remap) +
            source('port/linux/src/controller_layouts.c', layouts) + HELPERS +
            'int main(int argc,char **argv){(void)argc;data_root=argv[1];\n' + body + '\nreturn 0;}\n')


ROUND_TRIP = r'''
 struct controller_layout a=sample("''' + GUID + r'''"),b;char text[CONTROLLER_LAYOUT_FILE_MAXIMUM],problem[256];
 size_t n=controller_layout_format(&a,text,sizeof(text));
 assert(n && strstr(text,"input = \"left_paddle\"") && strstr(text,"legend = \"P3\"") &&
  strstr(text,"legend = \"RB2\"") && strstr(text,"shape = \"square\"") && strstr(text,"fill = \"#3070D0\"") &&
  strstr(text,"input = \"misc6\"") && strstr(text,"name = \"Turbo\""));
 /* (only the described: the touchpad's not written) */
 assert(!strstr(text,"touchpad"));
 assert(controller_layout_parse(text,n,&b,problem,sizeof(problem)));
 assert(!memcmp(&a,&b,sizeof(a)));
 /* kept by its GUID, and read back from the folder */
 assert(controller_layout_store(&a));
 {char name[64];snprintf(name,sizeof(name),"%s.toml",a.guid);assert(exists(name));}
 controller_layouts_scan();
 {struct controller_layout const *c=controller_layout_for("''' + GUID + r'''");
  assert(c && !memcmp(c,&a,sizeof(a)));}
 assert(!controller_layout_for("''' + OTHER + r'''") && !controller_layout_for("") && !controller_layout_for(NULL));
 /* a button's name in Button Remapping: its layout's, else its words */
 {struct controller_device d;memset(&d,0,sizeof(d));d.family=CONTROLLER_XBOX;
  snprintf(d.guid,sizeof(d.guid),"%s","''' + GUID + r'''");
  assert(!strcmp(controller_layout_button_name(CONTROLLER_REMAP_PADDLE+10,&d),"Turbo"));
  assert(!strcmp(controller_layout_button_name(CONTROLLER_REMAP_PADDLE+1,&d),"P1"));
  assert(!strcmp(controller_layout_button_name(0,&d),"A"));
  d.guid[0]=0;assert(!strcmp(controller_layout_button_name(CONTROLLER_REMAP_PADDLE,&d),"P3"));}
'''


def test_round_trip_and_kept_by_guid(tmp_path):
    run_c(tmp_path, program(ROUND_TRIP))


def test_round_trip_negative_control(tmp_path):
    """the fill written without its leading zeroes: read back as another color (or refused)"""
    faulty = (ROOT / 'port/linux/src/controller_layouts.c').read_text().replace('fill = \\"#%06lX\\"', 'fill = \\"#%lX\\"')
    assert faulty != (ROOT / 'port/linux/src/controller_layouts.c').read_text()
    body = ROUND_TRIP.replace('l.controls[3].fill=0x3070D0', 'l.controls[3].fill=0x0070D0').replace('#3070D0', '#70D0')
    assert run_c(tmp_path, program(body, layouts=faulty), expect_ok=False).returncode != 0


GOOD = 'format = 1\nguid = "' + GUID + '"\nname = "Pad"\n\n[[control]]\ninput = "left_paddle"\nname = "Left back"\n' \
       'legend = "LB"\nshape = "round"\nfill = "#D03030"\n'
# each a fault an imported file may have, and what its problem says
BAD = {
    'no format': ('format = 1\n', '', 'no format'),
    'format 2': ('format = 1', 'format = 2', 'a format'),
    'short guid': (GUID, GUID[:-1], 'a guid is 32 hex digits'),
    'guid not hex': (GUID, GUID[:-1] + 'g', 'a guid is 32 hex digits'),
    'unknown key': ('name = "Pad"\n', 'name = "Pad"\nexec = "rm"\n', "a key this file may not have"),
    'unknown table': ('[[control]]', '[[macro]]', 'a table'),
    'legend too long': ('legend = "LB"', 'legend = "LBXY"', 'a legend of 1 to 3'),
    'legend space': ('legend = "LB"', 'legend = "L B"', 'a legend of 1 to 3'),
    'no legend': ('legend = "LB"\n', '', 'has no legend'),
    'bad fill': ('#D03030', '#D0303', 'a fill is #RRGGBB'),
    'fill not hex': ('#D03030', '#D0303Z', 'a fill is #RRGGBB'),
    'bad shape': ('"round"', '"star"', 'a shape is round or square'),
    'a face button': ('"left_paddle"', '"south"', 'an input this file may not name'),
    'unknown input': ('"left_paddle"', '"paddle9"', 'an input this file may not name'),
    'input later': ('input = "left_paddle"\nname = "Left back"\n', 'name = "Left back"\ninput = "left_paddle"\n',
                    "a control's input comes first"),
    'input twice': ('fill = "#D03030"\n', 'fill = "#D03030"\n\n[[control]]\ninput = "left_paddle"\nlegend = "X"\n',
                    'an input given twice'),
    'key twice': ('shape = "round"\n', 'shape = "round"\nshape = "square"\n', 'a key given twice'),
    'backslash': ('"Left back"', '"Left \\\\back"', 'a backslash'),
    'quote in name': ('"Left back"', '"Left "back"', 'more after the value'),
    'control character': ('"Left back"', '"Left\x01back"', 'a name of 1 to 20'),
    'name too long': ('"Left back"', '"' + 'N' * 21 + '"', 'a name of 1 to 20'),
    'not a value': ('name = "Pad"', 'name = Pad', 'neither a string nor a number'),
    'no end quote': ('name = "Pad"', 'name = "Pad', 'a string with no end'),
    'nul byte': ('name = "Pad"', 'name = "P\\0ad"', 'not text'),
    'too many controls': ('fill = "#D03030"\n', 'fill = "#D03030"\n' + ''.join(
        f'\n[[control]]\ninput = "misc{n}"\nlegend = "M"\n' for n in range(1, 7)) + ''.join(
        f'\n[[control]]\ninput = "{x}"\nlegend = "X"\n' for x in ("right_paddle", "left_paddle2", "right_paddle2",
                                                                    "touchpad", "misc1")), 'more than 11 controls'),
}


def c_string(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\x01', '\\001') \
        .replace('\\\\0', '\\0') + '"'


@pytest.mark.parametrize('case', BAD)
def test_import_refuses_a_bad_file(tmp_path, case):
    """a file copied in with a fault is refused whole: not taken (no <guid>.toml), left where it is, its problem
    kept and logged; the folder's good layout still read"""
    before, after, words = BAD[case]
    assert before in GOOD
    bad = GOOD.replace(before, after, 1)
    body = r'''
 struct controller_layout a=sample("''' + OTHER + r'''"),parsed;char problem[256];
 static char const bad[]=''' + c_string(bad) + r''';
 assert(!controller_layout_parse(bad,sizeof(bad)-1,&parsed,problem,sizeof(problem)));
 printf("%s\n",problem);assert(strstr(problem,''' + c_string(words) + r'''));
 assert(controller_layout_store(&a));
 {FILE *f=fopen(at("shared.toml"),"wb");fwrite(bad,1,sizeof(bad)-1,f);fclose(f);}
 controller_layouts_scan();
 assert(!controller_layout_for("''' + GUID + r'''") && !exists("''' + GUID + r'''.toml") && exists("shared.toml"));
 assert(strstr(controller_layouts_problem(),"shared.toml: ") && strstr(logged,"refused"));
 assert(controller_layout_for("''' + OTHER + r'''"));
'''
    run_c(tmp_path, program(body))


def test_import_takes_a_copied_file(tmp_path):
    """a good file copied into the folder (any name) is its controller's layout from the next read: kept as
    <guid>.toml in place of the controller's own, the copy gone; a larger-than-allowed file refused unread"""
    body = r'''
 struct controller_layout own=sample("''' + GUID + r'''");struct controller_layout const *c;
 static char const good[]=''' + c_string(GOOD) + r''';
 own.controls[0].fill=0x30A040;assert(controller_layout_store(&own));
 put("from a friend.toml",good);
 controller_layouts_scan();
 c=controller_layout_for("''' + GUID + r'''");
 assert(c && !strcmp(c->controls[0].legend,"LB") && c->controls[0].fill==0xD03030 && !c->controls[3].name[0]);
 assert(!exists("from a friend.toml") && exists("''' + GUID + r'''.toml"));
 assert(!controller_layouts_problem()[0]);
 /* (too large: refused, never read) */
 {FILE *f=fopen(at("huge.toml"),"wb");int i;fputs(good,f);for(i=0;i<CONTROLLER_LAYOUT_FILE_MAXIMUM;i++)fputc('#',f);
  fclose(f);}
 controller_layouts_scan();
 assert(strstr(controller_layouts_problem(),"huge.toml") && exists("huge.toml"));
 /* (a <guid>.toml whose guid is another's: refused) */
 put("''' + OTHER + r'''.toml",good);
 controller_layouts_scan();
 assert(!controller_layout_for("''' + OTHER + r'''"));
'''
    run_c(tmp_path, program(body))


def test_import_negative_control(tmp_path):
    """the fill's digits not checked: a bad file taken, which the refusal test sees"""
    faulty = (ROOT / 'port/linux/src/controller_layouts.c').read_text().replace(
        'if (hex_digit(value[i]) < 0) return problem_set(problem, size, line, "a fill is #RRGGBB");', '')
    assert faulty != (ROOT / 'port/linux/src/controller_layouts.c').read_text()
    before, after, words = BAD['fill not hex']
    body = r'''
 struct controller_layout parsed;char problem[256];
 static char const bad[]=''' + c_string(GOOD.replace(before, after, 1)) + r''';
 assert(!controller_layout_parse(bad,sizeof(bad)-1,&parsed,problem,sizeof(problem)));
'''
    assert run_c(tmp_path, program(body, layouts=faulty), expect_ok=False).returncode != 0


@pytest.mark.parametrize('fill,legend', [(0x3070D0, 0xFFFFFF), (0xD03030, 0xFFFFFF), (0x707070, 0xFFFFFF),
                                         (0x8040C0, 0xFFFFFF), (0xE0C020, 0x000000), (0xE8E8E8, 0x000000),
                                         (0x30A040, 0x000000), (0xFFFFFF, 0x000000), (0x000000, 0xFFFFFF)])
def test_legend_color(tmp_path, fill, legend):
    """the legend reads on its fill: white on the dark ones, black on the light (WCAG's ratio, the higher)"""
    body = f' assert(controller_layout_legend_color(0x{fill:06X}UL)==0x{legend:06X}UL);'
    run_c(tmp_path, program(body))


@pytest.mark.parametrize('words,legend', [('P3', 'P3'), ('L4', 'L4'), ('LEFT BACK 2', 'LB2'), ('MISC 1', 'M1'),
                                          ('TOUCHPAD', 'T'), ('RIGHT STICK TOUCH', 'RST'), ('SHARE', 'S')])
def test_describe(tmp_path, words, legend):
    """a button described from its words: a short word its own legend, else its words' initials and digits"""
    body = (' struct controller_layout_control c;controller_layout_describe(&c,"' + words + '");\n'
            ' assert(!strcmp(c.name,"' + words + '") && !strcmp(c.legend,"' + legend + '") && c.shape==0 && '
            'c.fill==0x707070);')
    run_c(tmp_path, program(body))


# ---------- the prompts (controller_remap_game.c), with the layouts' lookup given

GAME = r'''
#include <wchar.h>
#define NONE -1
#define csmemcpy memcpy
#define csmemset memset
struct game_input_preferences {unsigned char game_control_to_xbox_buttons[12];};
static struct game_input_preferences profile;
static int console_is_active(void){return 0;}
void controller_remap_input_reset(void){}
int controller_remap_connected(short p){(void)p;return 1;}
unsigned int controller_remap_held(short p){(void)p;return 0;}
unsigned int controller_remap_pressed(short p,unsigned int m){(void)p;(void)m;return 0;}
static short local_player_get_next(short p){return p<1?1:NONE;}
static struct {short local_player_index;} fake_player={2};
static void input_abstraction_get_local_player_preferences(short port,struct game_input_preferences *p){(void)port;*p=profile;}
static int game_time_initialized(void){return 1;}
static int game_in_progress(void){return 1;}
static int main_menu_is_active(void){return 0;}
static int ui_widgets_port_active_for_everyone(void){return 0;}
static int ui_widgets_active_for_local_player(short port){(void)port;return 0;}
short network_chat_controller(void){return -1;}
static long local_player_get_player_index(short viewport){return viewport==1?7:NONE;}
#define player_get(index) (&fake_player)
static char device_guid[33];
void controller_device_get(short port,struct controller_device *d){memset(d,0,sizeof(*d));d->family=CONTROLLER_XBOX;
 if(port==2)snprintf(d->guid,sizeof(d->guid),"%s",device_guid);}
'''

PROMPT = r'''
 struct controller_layout a=sample("''' + GUID + r'''");struct controller_layout_control symbol;
 int p3=CONTROLLER_REMAP_PROMPT_PADDLE,p2=CONTROLLER_REMAP_PROMPT_PADDLE+3,misc6=CONTROLLER_REMAP_PROMPT_PADDLE+10;
 assert(controller_layout_store(&a));
 /* no controller (or one with no layout): no symbol, the words as before */
 assert(!controller_remap_prompt_symbol(1,p3,&symbol));
 {wchar_t const *w=controller_remap_prompt_text(1,p3);assert(w[0]=='P' && w[1]=='3' && !w[2]);}
 snprintf(device_guid,sizeof(device_guid),"%s","''' + OTHER + r'''");
 assert(!controller_remap_prompt_symbol(1,p2,&symbol));
 {wchar_t const *w=controller_remap_prompt_text(1,p2);assert(w[0]=='P' && w[1]=='2' && !w[2]);}
 /* its layout: the described buttons' symbols (viewport 1 is port 2's player), the others' words */
 snprintf(device_guid,sizeof(device_guid),"%s","''' + GUID + r'''");
 assert(controller_remap_prompt_symbol(1,p2,&symbol));
 assert(!strcmp(symbol.legend,"RB2") && symbol.shape==CONTROLLER_LAYOUT_SQUARE && symbol.fill==0x3070D0);
 assert(controller_remap_prompt_symbol(1,misc6,&symbol) && !strcmp(symbol.legend,"M6"));
 {wchar_t const *w=controller_remap_prompt_text(1,misc6);assert(w[0]=='T' && w[4]=='o' && !w[5]);}
 assert(!controller_remap_prompt_symbol(1,CONTROLLER_REMAP_PROMPT_PADDLE+4,&symbol));
 {wchar_t const *w=controller_remap_prompt_text(1,CONTROLLER_REMAP_PROMPT_PADDLE+4);assert(w[0]=='T' && w[7]=='D');}
 /* never a face button's icon, nor UNBOUND */
 assert(!controller_remap_prompt_symbol(1,0,&symbol) && !controller_remap_prompt_symbol(1,255,&symbol));
 assert(!controller_remap_prompt_text(1,0) && controller_remap_prompt_text(1,255)[0]=='U');
 /* another port's player keeps the words */
 assert(!controller_remap_prompt_symbol(0,p2,&symbol));
'''


def test_prompt_draws_the_chosen_symbol(tmp_path):
    run_c(tmp_path, program(PROMPT).replace(HELPERS, HELPERS + GAME + source(
        'port/linux/game/controller_remap_game.c')))


def test_prompt_negative_control(tmp_path):
    """the symbol looked up by the wrong button (the icon's index, not the extra's): the test sees it"""
    game = (ROOT / 'port/linux/game/controller_remap_game.c').read_text()
    faulty = game.replace('control = &layout->controls[button - CONTROLLER_REMAP_PROMPT_PADDLE];',
                          'control = &layout->controls[(button - CONTROLLER_REMAP_PROMPT_PADDLE + 1) % 11];')
    assert faulty != game
    code = program(PROMPT).replace(HELPERS, HELPERS + GAME + source('', faulty))
    assert run_c(tmp_path, code, expect_ok=False).returncode != 0


def test_prompt_sites_draw_the_symbol():
    """the HUD's messages and the help screens' prompts ask for the symbol before the words, and the key cap
    draws it in its fill, round or square, its legend in its own colour"""
    hud = (ROOT / 'source/interface/hud_messaging.c').read_text()
    widget = (ROOT / 'source/interface/ui_widget.c').read_text()
    assert re.search(r'if \(controller_glyph_layout_draw\(local_player_index, icon_index, &line_bounds,\s*'
                     r'&line_cursor, pixel_color\)\)', hud)
    assert hud.index('controller_glyph_layout_draw(local_player_index') < hud.index(
        'remapped ? remapped : L"<no button icon>"')
    assert 'controller_glyph_layout_draw(local_player_index_for_draw_string_and_hack_in_icons, icon_index,' in widget
    prompts = (ROOT / 'port/linux/game/hud_key_prompts.c').read_text()
    assert 'fill = (color & 0xFF000000UL) | (key_cap_symbol_fill & 0x00FFFFFFUL);' in prompts
    assert 'key_cap_symbol && key_cap_symbol_round ? floorf(height * 0.5f)' in prompts
    glyphs = (ROOT / "port/linux/game/controller_layout_prompts.c").read_text()
    assert 'controller_layout_legend_color(symbol.fill), symbol.shape == CONTROLLER_LAYOUT_ROUND' in glyphs


# ---------- sharing (#1144): export with or without the bindings, their import refused or taken, applied by choice

SHARE = r'''
 struct controller_layout a=sample("''' + GUID + r'''"),b;struct controller_layout const *c;
 unsigned char mine[CONTROLLER_REMAP_ACTIONS],theirs[CONTROLLER_REMAP_ACTIONS];char written[80];
 static char text[20000];
 FILE *f;size_t n;int differ;
 controller_remap_defaults(mine);memcpy(theirs,mine,sizeof(mine));
 controller_remap_assign(theirs,PAD_JUMP,CONTROLLER_REMAP_PADDLE);      /* jump on the left paddle */
 controller_remap_assign(theirs,PAD_MELEE,CONTROLLER_REMAP_PADDLE+3);  /* melee on the second right */
 /* without the bindings (INCLUDE MY BINDINGS off, the default): none in the file */
 assert(controller_layout_export(&a,NULL,written,sizeof(written)));
 assert(!strcmp(written,"controller_layouts/shared/Fight stick.toml"));
 f=fopen(at("shared/Fight stick.toml"),"rb");assert(f);n=fread(text,1,sizeof(text)-1,f);fclose(f);text[n]=0;
 assert(!strstr(text,"[bindings]") && strstr(text,"legend = \"RB2\""));
 /* with them: every action, each its button's token, read back the same */
 assert(controller_layout_export(&a,theirs,written,sizeof(written)));
 f=fopen(at("shared/Fight stick.toml"),"rb");assert(f);n=fread(text,1,sizeof(text)-1,f);fclose(f);text[n]=0;
 assert(strstr(text,"[bindings]\njump = \"left_paddle\"\n") && strstr(text,"melee = \"right_paddle2\"") &&
  strstr(text,"ping = \"dpad_right\""));
 assert(controller_layout_parse(text,n,&b,written,sizeof(written)));
 assert(b.bound==(1ul<<CONTROLLER_REMAP_ACTIONS)-1 && !memcmp(b.bindings,theirs,sizeof(theirs)));
 /* (the shared folder is not read as layouts) */
 controller_layouts_scan();assert(!controller_layout_for("''' + GUID + r'''"));
 /* copied in: taken, its bindings kept with it, never put on anyone's buttons by that */
 put("from a friend.toml",text);controller_layouts_scan();
 c=controller_layout_for("''' + GUID + r'''");assert(c && c->bound==b.bound);
 assert(!controller_remap_read(0,mine) && !controller_remap_read(1,mine));
 /* chosen: through the assign rule, the actions a button is taken from left unbound */
 controller_remap_defaults(mine);
 differ=controller_layout_bindings_apply(c,mine);
 assert(differ>=2 && mine[PAD_JUMP]==CONTROLLER_REMAP_PADDLE && mine[PAD_MELEE]==CONTROLLER_REMAP_PADDLE+3);
 assert(!memcmp(mine,theirs,sizeof(mine)) && controller_layout_bindings_apply(c,mine)==0);
 /* a partial [bindings]: only its actions; one whose button another has takes it */
 {static char const part[]="format = 1\nguid = \"''' + GUID + r'''\"\n[bindings]\nfire = \"south\"\n";
  assert(controller_layout_parse(part,sizeof(part)-1,&b,written,sizeof(written)) && b.bound==1ul<<PAD_FIRE);
  controller_remap_defaults(mine);assert(controller_layout_bindings_apply(&b,mine)==2);
  assert(mine[PAD_FIRE]==0 && mine[PAD_JUMP]==CONTROLLER_REMAP_UNBOUND);}
'''


def test_export_bindings_only_when_chosen_and_applied_by_choice(tmp_path):
    run_c(tmp_path, program(SHARE))


BAD_BINDINGS = {
    'unknown action': ('jump = "south"', 'teleport = "south"', 'an action this game has not'),
    'unknown button': ('jump = "south"', 'jump = "start"', 'a button this game has not'),
    'not a string': ('jump = "south"', 'jump = 3', 'a button this game has not'),
    'action twice': ('jump = "south"\n', 'jump = "south"\njump = "east"\n', 'an action given twice'),
    'two on one button': ('melee = "east"', 'melee = "south"', 'two actions on south'),
    'second table': ('melee = "east"\n', 'melee = "east"\n[bindings]\n', 'a second [bindings]'),
}
BINDINGS_FILE = GOOD + '\n[bindings]\njump = "south"\nmelee = "east"\ncamera_up = "south"\n'


@pytest.mark.parametrize('case', BAD_BINDINGS)
def test_import_refuses_bad_bindings(tmp_path, case):
    """bindings that are not Button Remapping's refuse the whole file (its symbols too); a debug camera action may
    share a gameplay one's button, as Button Remapping allows"""
    before, after, words = BAD_BINDINGS[case]
    body = r'''
 struct controller_layout parsed;char problem[256];
 static char const good[]=''' + c_string(BINDINGS_FILE) + r''',bad[]=''' + c_string(
        BINDINGS_FILE.replace(before, after, 1)) + r''';
 assert(controller_layout_parse(good,sizeof(good)-1,&parsed,problem,sizeof(problem)));
 assert(parsed.bound==((1ul<<PAD_JUMP)|(1ul<<PAD_MELEE)|(1ul<<PAD_CAMERA_UP)));
 assert(!controller_layout_parse(bad,sizeof(bad)-1,&parsed,problem,sizeof(problem)));
 printf("%s\n",problem);assert(strstr(problem,''' + c_string(words) + r'''));
 put("shared.toml",bad);controller_layouts_scan();
 assert(!controller_layout_for("''' + GUID + r'''") && exists("shared.toml"));
'''
    run_c(tmp_path, program(body))


def test_bindings_negative_control(tmp_path):
    """the group check dropped: two actions on one button taken, which the refusal test sees"""
    layouts = (ROOT / 'port/linux/src/controller_layouts.c').read_text()
    faulty = layouts.replace('if (controller_remap_conflict(buttons, action, buttons[action]) >= 0)', 'if (0)')
    assert faulty != layouts
    before, after, words = BAD_BINDINGS['two on one button']
    body = r'''
 struct controller_layout parsed;char problem[256];
 static char const bad[]=''' + c_string(BINDINGS_FILE.replace(before, after, 1)) + r''';
 assert(!controller_layout_parse(bad,sizeof(bad)-1,&parsed,problem,sizeof(problem)));
'''
    assert run_c(tmp_path, program(body, layouts=faulty), expect_ok=False).returncode != 0


def test_bindings_used_only_when_chosen():
    """a layout's bindings reach a player's buttons only through the question's USE THIS LAYOUT'S (never as a
    controller connects or the folder is read), and INCLUDE MY BINDINGS is off by default"""
    menu = (ROOT / 'port/linux/game/controller_layout_menu.inc').read_text()
    assert menu.count('layout_bindings_use()') == 1  # the action's call alone
    assert 'if (!strcmp(name, "layout bindings use"))\n\t\treturn layout_bindings_use();' in menu
    for path in ('port/linux/src/controller_layouts.c', 'port/linux/game/controller_remap_game.c',
                 'port/linux/src/xinput_sdl.c', 'port/linux/src/controller_buttons.c'):
        assert 'layout_bindings_use' not in (ROOT / path).read_text()
    assert 'config_write' not in (ROOT / 'port/linux/src/controller_layouts.c').read_text()
    assert '{ "input.controller_layout_bindings", _config_boolean, "false",' in \
        (ROOT / 'port/linux/src/port_config.c').read_text()
    question = (ROOT / 'port/assets/menus/screens/layout_bindings.xml').read_text()
    assert '<list focus="keep">' in question and 'id="keep" label="KEEP MINE" do="back"' in question
