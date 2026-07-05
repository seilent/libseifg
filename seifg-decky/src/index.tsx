import { callable, definePlugin } from "@decky/api";
import {
  PanelSection,
  PanelSectionRow,
  SliderField,
  ButtonItem,
  ToggleField,
  DropdownItem,
} from "@decky/ui";
import { useState, useEffect } from "react";
import { FaBolt } from "react-icons/fa";

declare const SteamClient: {
  GameSessions: {
    RegisterForAppLifetimeNotifications: (
      cb: (e: { unAppID: number; bRunning: boolean }) => void
    ) => { unregister: () => void };
  };
};

declare const appStore: {
  GetAppOverviewByAppID: (appId: number) => { display_name: string } | null;
};

interface Status {
  installed: boolean;
}

interface Settings {
  enabled: boolean;
  multiplier: number;
  target_fps: number;
}

const getStatus = callable<[], Status>("get_status");
const install = callable<[], boolean>("install");
const uninstall = callable<[], boolean>("uninstall");
const getGameSettings = callable<[appId: string], Settings>("get_game_settings");
const saveGameSettings = callable<[appId: string, settings: string], boolean>("save_game_settings");
const getDefaultSettings = callable<[], Settings>("get_default_settings");
const saveDefaultSettings = callable<[settings: string], boolean>("save_default_settings");

const MULTIPLIER_OPTIONS = [
  { data: 2, label: "2x" },
  { data: 3, label: "3x" },
];

const FPS_OPTIONS = [
  { data: 30, label: "30" },
  { data: 40, label: "40" },
  { data: 60, label: "60" },
  { data: 120, label: "120" },
];

const state = {
  runningAppId: 0,
  runningGameName: "",
};

function Content() {
  const [status, setStatus] = useState<Status | null>(null);
  const [settings, setSettings] = useState<Settings | null>(null);
  const [appId, setAppId] = useState(state.runningAppId);
  const [gameName, setGameName] = useState(state.runningGameName);
  const [working, setWorking] = useState(false);
  const [copied, setCopied] = useState(false);

  const refresh = async () => {
    const s = await getStatus();
    setStatus(s);
  };

  const loadSettings = async (id: number) => {
    const cfg = id > 0
      ? await getGameSettings(String(id))
      : await getDefaultSettings();
    setSettings(cfg);
  };

  useEffect(() => {
    refresh();
    loadSettings(state.runningAppId);
    const interval = setInterval(() => {
      if (state.runningAppId !== appId) {
        setAppId(state.runningAppId);
        setGameName(state.runningGameName);
        loadSettings(state.runningAppId);
      }
    }, 1000);
    return () => clearInterval(interval);
  }, []);

  const save = async (updated: Settings) => {
    setSettings(updated);
    if (appId > 0) {
      await saveGameSettings(String(appId), JSON.stringify(updated));
    } else {
      await saveDefaultSettings(JSON.stringify(updated));
    }
  };

  if (!status) {
    return (
      <PanelSection title="SeiFG">
        <PanelSectionRow>
          <div>Loading...</div>
        </PanelSectionRow>
      </PanelSection>
    );
  }

  return (
    <div>
      <PanelSection title="SeiFG">
        <PanelSectionRow>
          <div style={{ fontSize: "12px", opacity: 0.8 }}>
            {status.installed ? "Installed" : "Not installed"}
          </div>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            disabled={working}
            onClick={async () => {
              setWorking(true);
              if (status.installed) {
                await uninstall();
              } else {
                await install();
              }
              await refresh();
              setWorking(false);
            }}
          >
            {working ? "..." : status.installed ? "Uninstall" : "Install"}
          </ButtonItem>
        </PanelSectionRow>
      </PanelSection>

      {status.installed && settings && (
        <PanelSection title={appId > 0 ? (gameName || `App ${appId}`) : "Defaults"}>
          <PanelSectionRow>
            <ToggleField
              label="Frame Generation"
              checked={settings.enabled}
              onChange={(v) => save({ ...settings, enabled: v })}
            />
          </PanelSectionRow>
          {settings.enabled && (
            <>
              <PanelSectionRow>
                <DropdownItem
                  label="Multiplier"
                  rgOptions={MULTIPLIER_OPTIONS}
                  selectedOption={settings.multiplier}
                  onChange={(opt) => save({ ...settings, multiplier: opt.data as number })}
                />
              </PanelSectionRow>
              <PanelSectionRow>
                <DropdownItem
                  label="Target FPS"
                  rgOptions={FPS_OPTIONS}
                  selectedOption={settings.target_fps}
                  onChange={(opt) => save({ ...settings, target_fps: opt.data as number })}
                />
              </PanelSectionRow>
              <PanelSectionRow>
                <div style={{ fontSize: "11px", opacity: 0.6 }}>
                  Output: {settings.target_fps} FPS, DXVK cap: {Math.max(1, Math.round(settings.target_fps / settings.multiplier))}
                </div>
              </PanelSectionRow>
            </>
          )}
        </PanelSection>
      )}

      {status.installed && (
        <PanelSection title="Launch Option">
          <PanelSectionRow>
            <ButtonItem
              layout="below"
              onClick={() => {
                const input = document.createElement("input");
                input.value = "~/seifg %command%";
                input.style.position = "absolute";
                input.style.left = "-9999px";
                document.body.appendChild(input);
                input.focus();
                input.select();
                document.execCommand("copy");
                document.body.removeChild(input);
                setCopied(true);
                setTimeout(() => setCopied(false), 2000);
              }}
            >
              {copied ? "Copied" : "Copy: ~/seifg %command%"}
            </ButtonItem>
          </PanelSectionRow>
          <PanelSectionRow>
            <div style={{ fontSize: "11px", opacity: 0.5 }}>
              Paste into game Properties &gt; Launch Options
            </div>
          </PanelSectionRow>
        </PanelSection>
      )}
    </div>
  );
}

export default definePlugin(() => {
  const reg = SteamClient.GameSessions.RegisterForAppLifetimeNotifications((e) => {
    if (e.bRunning) {
      state.runningAppId = e.unAppID;
      const app = appStore.GetAppOverviewByAppID(e.unAppID);
      state.runningGameName = app?.display_name ?? String(e.unAppID);
    } else if (e.unAppID === state.runningAppId) {
      state.runningAppId = 0;
      state.runningGameName = "";
    }
  });

  return {
    name: "SeiFG",
    content: <Content />,
    icon: <FaBolt />,
    onDismount() {
      reg.unregister();
    },
  };
});
