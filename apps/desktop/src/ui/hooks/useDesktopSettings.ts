import { useCallback, useEffect, useState } from "react";
import { defaultPort, type PersistedFieldPatch } from "../appTypes";

export function useDesktopSettings() {
  const [tcpHost, setTcpHost] = useState("192.168.4.42");
  const [tcpPort, setTcpPort] = useState(defaultPort);
  const [wifiSsid, setWifiSsid] = useState("");
  const [wifiPassword, setWifiPassword] = useState("");
  const [savedWifiSsid, setSavedWifiSsid] = useState("");
  const [savedWifiPassword, setSavedWifiPassword] = useState("");

  useEffect(() => {
    void window.autoTyper?.readStore().then((store) => {
      if (store.lastTcpHost) {
        setTcpHost(store.lastTcpHost);
      }
      if (store.lastTcpPort) {
        setTcpPort(store.lastTcpPort);
      }
      if (store.savedWifiSsid) {
        setWifiSsid(store.savedWifiSsid);
        setSavedWifiSsid(store.savedWifiSsid);
      }
      if (store.savedWifiPassword) {
        setWifiPassword(store.savedWifiPassword);
        setSavedWifiPassword(store.savedWifiPassword);
      }
    });
  }, []);

  const persistFields = useCallback(async (next: PersistedFieldPatch) => {
    const current = await window.autoTyper?.readStore();
    if (!current || !window.autoTyper) {
      return;
    }
    await window.autoTyper.writeStore({
      ...current,
      ...next,
    });
    if (next.savedWifiSsid !== undefined) {
      setSavedWifiSsid(next.savedWifiSsid);
    }
    if (next.savedWifiPassword !== undefined) {
      setSavedWifiPassword(next.savedWifiPassword);
    }
  }, []);

  return {
    tcpHost,
    setTcpHost,
    tcpPort,
    setTcpPort,
    wifiSsid,
    setWifiSsid,
    wifiPassword,
    setWifiPassword,
    savedWifiSsid,
    savedWifiPassword,
    persistFields,
  };
}
