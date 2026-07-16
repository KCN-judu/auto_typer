import { useCallback, useState } from "react";

const initialLogLine = "等待 TCP 连接";

export function useTaskLog() {
  const [logLines, setLogLines] = useState<string[]>([initialLogLine]);

  const appendLog = useCallback((line: string) => {
    setLogLines((lines) => [...lines, `${new Date().toLocaleTimeString()} ${line}`].slice(-12));
  }, []);

  return {
    logLines,
    appendLog,
  };
}
