"""One-time remote evidence; not part of the product PR."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET

preset, backend = os.environ['PRESET'], os.environ['BACKEND']
build = Path('build') / preset
out = Path('evidence').resolve()
out.mkdir(exist_ok=True)
scenes = ['ui_rect_border', 'ui_nine_slice', 'ui_clip_nested', 'ui_z_popup', 'ui_text_labels',
          'ui_widgets_basic', 'ui_widgets_advanced_text', 'ui_widgets_advanced_controls',
          'ui_world_1m', 'ui_world_20m', 'ui_world_occluded', 'ui_world_overlay',
          'ui_world_lit', 'ui_world_origin_100km', 'ui_pixel_four']
new_case = r'^ui_layout\.ui_advanced\.tooltip_measures_wrapped_text_and_clips_to_screen_and_panel_bounds$'
cpu_cases = r'^ui_layout\.ui_advanced\.(generated_.*|dropdown_flips_selects_once_and_cancel_preserves_value|tabs_generate_headers_hide_inactive_pages_and_emit_change|tooltip_delay_focus_safe_bounds_and_press_hide|tooltip_measures_wrapped_text_and_clips_to_screen_and_panel_bounds|actual_escape_and_outside_click_close_dropdown_without_selection)$|^ui_world_pixel\.ui_world\.(per_root_panel_pixels_preserve_screen_root|popups_use_panel_extent_scale_and_ignore_screen_safe_insets|popup_percent_and_oversize_clamps_are_independent_between_panels)$'
pattern = r'^golden\.' + backend + r'\.(' + '|'.join(scenes) + ')$'

def checked(args):
    subprocess.run(args, check=True)

def ctest(pattern, directory, name, env=None):
    directory.mkdir(parents=True, exist_ok=True)
    args = ['ctest', '--test-dir', str(build), '-R', pattern, '--parallel', '1',
            '--no-tests=error', '--output-on-failure', '--test-output-size-passed', '1048576',
            '--test-output-size-failed', '1048576', '--output-junit', str(directory / (name + '.xml'))]
    result = subprocess.run(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (directory / (name + '.log')).write_text(result.stdout)
    cases = ET.parse(directory / (name + '.xml')).findall('.//testcase')
    assert cases and all(c.find('skipped') is None and c.find('error') is None for c in cases)
    print(name, 'exit', result.returncode, 'cases', len(cases), flush=True)
    return result.returncode, cases

(out / 'head.txt').write_text(subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True))
checked(['git', 'restore', '--source=7777e995546b7ccea2e045b8e060de048e997b5b', '--', 'src/ui/widgets/tooltip.c'])
(out / 'original-runtime.diff').write_text(subprocess.check_output(['git', 'diff'], text=True))
checked(['cmake', '--preset', preset])
checked(['cmake', '--build', '--preset', preset, '--target', 'de_golden', 'test_ui_layout',
         'test_ui_world_pixel', 'test_ui_popup_native'])
code, cases = ctest(new_case, out / 'before', 'cpu')
assert code != 0 and len(cases) == 1 and cases[0].find('failure') is not None
assert 'tooltip variant' in (cases[0].findtext('system-out') or '')
manifest = {}
for phase in ('before', 'after'):
    if phase == 'after':
        checked(['git', 'restore', '--', 'src/ui/widgets/tooltip.c'])
        checked(['git', 'diff', '--exit-code'])
        checked(['cmake', '--build', '--preset', preset, '--target', 'de_golden', 'test_ui_layout',
                 'test_ui_world_pixel', 'test_ui_popup_native'])
    directory = out / phase
    code, cases = ctest(pattern, directory, 'goldens')
    shutil.copytree(build / 'golden' / backend / 'base', directory / 'images', dirs_exist_ok=True)
    shutil.copyfile(build / 'Testing/Temporary/LastTest.log', directory / 'native-LastTest.log')
    assert code != 0 and len(cases) == len(scenes)
    assert {c.attrib['name'].split('.', 2)[2] for c in cases} == set(scenes)
    for c in cases:
        output = c.findtext('system-out') or ''
        assert c.find('failure') is not None and ': no reference ' in output, c.attrib
        assert '[error]' not in output and '[fatal]' not in output, c.attrib
    manifest[phase] = {}
    for scene in scenes:
        image = directory / 'images' / (scene + '.png')
        data = image.read_bytes()
        assert data[:8] == b'\x89PNG\r\n\x1a\n'
        dimensions = struct.unpack('>II', data[16:24])
        assert dimensions == ((1920, 1080) if scene == 'ui_pixel_four' else (1280, 720))
        manifest[phase][scene] = {'sha256': hashlib.sha256(data).hexdigest(), 'dimensions': dimensions}
for scene in scenes:
    equal = manifest['before'][scene]['sha256'] == manifest['after'][scene]['sha256']
    assert equal == (scene != 'ui_widgets_advanced_controls'), (scene, equal)
code, cases = ctest(cpu_cases, out / 'after', 'cpu')
assert code == 0 and len(cases) == 12 and all(c.find('failure') is None for c in cases)
env = os.environ.copy()
env['DE_UI_POPUP_NATIVE_BACKEND'] = backend
env['DE_UI_POPUP_EVIDENCE_DIR'] = str(out / 'after')
code, cases = ctest(r'^ui_popup_native\.generated_widgets_stay_on_world_panel$', out / 'after', 'popup', env)
assert code == 0 and len(cases) == 1 and cases[0].find('failure') is None
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('Verified 14 unchanged native images, 1 changed tooltip scene, 12 CPU + 1 native popup passes.', flush=True)
