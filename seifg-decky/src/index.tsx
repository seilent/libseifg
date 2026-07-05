import { callable, definePlugin } from "@decky/api";
import {
  PanelSection,
  PanelSectionRow,
  SliderField,
  ButtonItem,
  ToggleField,
} from "@decky/ui";
import { useState, useEffect } from "react";

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
const getDisplayHz = callable<[], { hz: number }>("get_display_hz");
const install = callable<[], boolean>("install");
const uninstall = callable<[], boolean>("uninstall");
const getGameSettings = callable<[appId: string], Settings>("get_game_settings");
const saveGameSettings = callable<[appId: string, settings: string], boolean>("save_game_settings");
const getDefaultSettings = callable<[], Settings>("get_default_settings");
const saveDefaultSettings = callable<[settings: string], boolean>("save_default_settings");

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
  const [maxHz, setMaxHz] = useState(60);

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
    getDisplayHz().then((r) => setMaxHz(r.hz || 60));
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
    const capped = { ...updated, multiplier: maxHz > 60 ? updated.multiplier : 2 };
    setSettings(capped);
    if (appId > 0) {
      await saveGameSettings(String(appId), JSON.stringify(capped));
    } else {
      await saveDefaultSettings(JSON.stringify(capped));
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
                {maxHz > 60 ? (
                  <SliderField
                    label="Multiplier"
                    value={settings.multiplier}
                    min={2}
                    max={3}
                    step={1}
                    notchCount={2}
                    notchLabels={[
                      { notchIndex: 0, label: "2x" },
                      { notchIndex: 1, label: "3x" },
                    ]}
                    notchTicksVisible={true}
                    onChange={(v) => save({ ...settings, multiplier: v })}
                  />
                ) : (
                  <div style={{ fontSize: "12px", opacity: 0.8 }}>
                    Multiplier: 2x (3x needs a &gt;60Hz display)
                  </div>
                )}
              </PanelSectionRow>
              <PanelSectionRow>
                <SliderField
                  label="Target FPS"
                  value={settings.target_fps}
                  min={30}
                  max={120}
                  step={10}
                  showValue={true}
                  onChange={(v) => save({ ...settings, target_fps: v })}
                />
              </PanelSectionRow>
              <PanelSectionRow>
                <div style={{ fontSize: "11px", opacity: 0.6 }}>
                  Output: {settings.target_fps} FPS, DXVK cap: {Math.max(1, Math.round(settings.target_fps / (maxHz > 60 ? settings.multiplier : 2)))}
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
    icon: <span style={{ fontWeight: 700, fontSize: "14px" }}>FG</span>,
    onDismount() {
      reg.unregister();
    },
  };
});
