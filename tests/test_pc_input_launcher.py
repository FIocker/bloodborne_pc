import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pc_input_config', ROOT / 'launcher/bbport_input_config.py')
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


class PcInputLauncherTests(unittest.TestCase):
    def test_existing_bindings_enable_direct_without_rewriting_them(self):
        lines = ['cross = space', 'r3 = q', 'r3 = c', 'mouse_to_joystick = right']
        bindings, original, *_rest, mouse = config.parse_input_lines(lines)
        self.assertEqual(original, lines)
        self.assertEqual(bindings['r3'], ['q', 'c'])
        self.assertEqual(mouse['camera_mode'], 'direct')
        self.assertTrue(mouse['enabled'])

    def test_old_multiplier_is_separate_from_ds3_sensitivity(self):
        *_, mouse = config.parse_input_lines([
            'mouse_camera_sensitivity=1.25', 'mouse_direct_sensitivity=7',
            'mouse_camera_invert_y=1', 'mouse_hold_auto_rotation=off'])
        self.assertEqual(mouse['camera_sensitivity'], 1.25)
        self.assertEqual(mouse['direct_sensitivity'], 7)
        self.assertTrue(mouse['invert_y'])
        self.assertFalse(mouse['hold_auto_rotation'])

    def test_explicit_legacy_and_disabled_modes_survive(self):
        *_, mouse = config.parse_input_lines(['mouse_to_joystick=right', 'mouse_camera=stick',
                                              'mouse_look_feel=legacy'])
        self.assertEqual(mouse['camera_mode'], 'stick')
        self.assertEqual(mouse['look_feel'], 'legacy')
        *_, disabled = config.parse_input_lines(['mouse_to_joystick=right', 'mouse_camera=off'])
        self.assertFalse(disabled['enabled'])

    def test_nonfinite_and_out_of_range_settings_do_not_enter_widgets(self):
        for invalid in ('nan', 'inf', '-inf', 'eleven', '11', '-1'):
            with self.subTest(invalid=invalid):
                *_, mouse = config.parse_input_lines([f'mouse_direct_sensitivity={invalid}'])
                self.assertEqual(mouse['direct_sensitivity'], 5)

    def test_round_trip_keeps_hotkeys_controller_and_unknown_lines(self):
        lines = ['hotkey_toggle_mouse_to_joystick=f6', 'cross=cross', '# my controller',
                 'mouse_to_joystick=right', 'mouse_camera=stick', 'mouse_camera=direct',
                 'future_input_feature=kept']
        *_, mouse = config.parse_input_lines(lines)
        mouse['direct_sensitivity'] = 9
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'input.ini'
            config.save_mouse_settings(path, mouse, lines)
            text = path.read_text()
            *_, loaded = config.load_input_ini(path)
        self.assertIn('hotkey_toggle_mouse_to_joystick=f6', text)
        self.assertIn('cross=cross', text)
        self.assertIn('future_input_feature=kept', text)
        self.assertEqual(text.count('mouse_camera ='), 1)
        self.assertEqual(loaded['direct_sensitivity'], 9)

    def test_preset_removes_conflicting_secondary_pc_binding_but_keeps_pad(self):
        lines = ['r3=q', 'r3=c', 'r3=r3', 'r1=r1', 'mouse_camera=direct', '# custom pad']
        output = config.bbhost_preset_lines(lines)
        bindings, *_ = config.parse_input_lines(output)
        self.assertEqual(bindings['r3'], ['q'])
        self.assertEqual(bindings['l1'], ['c', 'middlebutton'])
        self.assertEqual(bindings['strong'], ['lshift'])
        self.assertEqual(bindings['menu_confirm'], ['enter'])
        self.assertEqual(bindings['menu_back'], ['backspace'])
        self.assertIn('r3=r3', output)
        self.assertIn('r1=r1', output)
        self.assertIn('# custom pad', output)

    def test_preset_can_be_applied_twice_without_duplicate_bindings(self):
        lines = config.bbhost_preset_lines(config.bbhost_preset_lines(['cross=cross']))
        bindings, *_ = config.parse_input_lines(lines)
        self.assertEqual(bindings['cross'], ['e'])
        self.assertEqual(bindings['l1'], ['c', 'middlebutton'])
        self.assertEqual(lines.count('cross=cross'), 1)


if __name__ == '__main__':
    unittest.main()
