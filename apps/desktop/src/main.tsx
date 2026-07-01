import React from "react";
import { createRoot } from "react-dom/client";
import { App } from "./ui/App";
import "./styles/app.css";
import "./styles/dashboard.css";
import "./styles/keymap.css";

createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
