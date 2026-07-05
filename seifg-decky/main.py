import os
import json
import shutil
import stat
import decky

HOME = os.path.expanduser("~")
CONFIG_DIR = os.path.join(HOME, ".config", "seifg")
GAMES_DIR = os.path.join(CONFIG_DIR, "games")
DEFAULT_CONF = os.path.join(CONFIG_DIR, "default.json")

LAYER_DIR = os.path.join(HOME, "seifg-vk")
LAYER_SO = os.path.join(LAYER_DIR, "libseifg-vk.so")
MANIFEST_DIR = os.path.join(HOME, ".local", "share", "vulkan", "implicit_layer.d")
MANIFEST_PATH = os.path.join(MANIFEST_DIR, "VkLayer_SEIFG_frame_generation.json")
WRAPPER_PATH = os.path.join(HOME, "seifg")
FEX_CONFIG = os.path.join(HOME, ".config", "fex-emu", "Config.json")

DEFAULT_SETTINGS = {
    "enabled": False,
    "multiplier": 2,
    "target_fps": 60,
}


def _load_json(path):
    if os.path.exists(path):
        try:
            with open(path, "r") as f:
                return json.load(f)
        except Exception:
            pass
    return None


def _save_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2)


def _is_installed():
    return (
        os.path.isfile(LAYER_SO)
        and os.path.isfile(MANIFEST_PATH)
        and os.path.isfile(WRAPPER_PATH)
    )


class Plugin:

    async def get_status(self):
        return {"installed": _is_installed()}

    async def install(self):
        plugin_dir = decky.DECKY_PLUGIN_DIR
        bundled_so = os.path.join(plugin_dir, "bin", "libseifg-vk.so")
        bundled_wrapper = os.path.join(plugin_dir, "defaults", "seifg")

        os.makedirs(LAYER_DIR, exist_ok=True)
        shutil.copy2(bundled_so, LAYER_SO)

        os.makedirs(MANIFEST_DIR, exist_ok=True)
        manifest = {
            "file_format_version": "1.0.0",
            "layer": {
                "name": "VK_LAYER_SEIFG_frame_generation",
                "type": "GLOBAL",
                "library_path": LAYER_SO,
                "api_version": "1.3.0",
                "implementation_version": "1",
                "description": "SeiFG frame generation layer",
                "enable_environment": {"SEIFG_ENABLE": "1"},
                "disable_environment": {"SEIFG_DISABLE": "1"},
            },
        }
        _save_json(MANIFEST_PATH, manifest)

        shutil.copy2(bundled_wrapper, WRAPPER_PATH)
        os.chmod(WRAPPER_PATH, os.stat(WRAPPER_PATH).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

        os.makedirs(CONFIG_DIR, exist_ok=True)
        os.makedirs(GAMES_DIR, exist_ok=True)
        if not os.path.exists(DEFAULT_CONF):
            _save_json(DEFAULT_CONF, dict(DEFAULT_SETTINGS))

        return True

    async def uninstall(self):
        if os.path.isfile(LAYER_SO):
            os.remove(LAYER_SO)
        if os.path.isdir(LAYER_DIR) and not os.listdir(LAYER_DIR):
            os.rmdir(LAYER_DIR)
        if os.path.isfile(MANIFEST_PATH):
            os.remove(MANIFEST_PATH)
        if os.path.isfile(WRAPPER_PATH):
            os.remove(WRAPPER_PATH)
        return True

    async def get_game_settings(self, app_id: str):
        if not app_id or app_id == "0":
            cfg = _load_json(DEFAULT_CONF)
            return cfg if cfg else dict(DEFAULT_SETTINGS)
        path = os.path.join(GAMES_DIR, f"{app_id}.json")
        cfg = _load_json(path)
        if cfg is not None:
            return cfg
        fallback = _load_json(DEFAULT_CONF)
        return fallback if fallback else dict(DEFAULT_SETTINGS)

    async def save_game_settings(self, app_id: str, settings: str):
        path = os.path.join(GAMES_DIR, f"{app_id}.json")
        _save_json(path, json.loads(settings))
        return True

    async def get_default_settings(self):
        cfg = _load_json(DEFAULT_CONF)
        return cfg if cfg else dict(DEFAULT_SETTINGS)

    async def save_default_settings(self, settings: str):
        _save_json(DEFAULT_CONF, json.loads(settings))
        return True

    async def list_game_profiles(self):
        profiles = []
        if os.path.isdir(GAMES_DIR):
            for f in os.listdir(GAMES_DIR):
                if f.endswith(".json"):
                    profiles.append(f[:-5])
        return profiles

    async def delete_game_profile(self, app_id: str):
        path = os.path.join(GAMES_DIR, f"{app_id}.json")
        if os.path.exists(path):
            os.remove(path)
        return True

    async def _main(self):
        os.makedirs(CONFIG_DIR, exist_ok=True)
        os.makedirs(GAMES_DIR, exist_ok=True)

    async def _unload(self):
        pass
