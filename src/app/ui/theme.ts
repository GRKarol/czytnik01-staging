import { css } from "lit";

/**
 * The reader's own look (Nano UI): black screen, dark rounded tiles, one
 * accent color, Inter. The accent is the reader's highlight color once the
 * app knows it (the shell sets --accent); before that the Flower blue.
 *
 * The older variable names (--ink, --paper, --line, --fr ...) stay as
 * aliases, so every component in the tree picks the theme up through the
 * shadow DOM boundary.
 */
export const themeTokens = css`
  :host {
    --bg: #000000;
    --surface: #141518;
    --surface-2: #1d1e23;
    --surface-3: #2a2c33;
    --outline: #34363e;
    --text: #f2f2f4;
    --text-2: #b7b9c2;
    --muted: #8a8d98;
    --accent: #3d5afe;
    --accent-text: color-mix(in srgb, var(--accent) 62%, white);
    --accent-soft: color-mix(in srgb, var(--accent) 18%, transparent);
    --on-accent: #ffffff;
    --ok: #4cc38a;
    --warn: #e3b355;
    --err: #ff6b5e;
    --radius-lg: 18px;
    --radius: 14px;
    --radius-sm: 10px;
    --radius-pill: 999px;
    --font: "Inter", system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
    --font-mono: ui-monospace, "SF Mono", "Roboto Mono", Menlo, monospace;
    --safe-top: max(env(safe-area-inset-top, 0px), var(--safe-area-inset-top, 0px));
    --safe-bottom: max(env(safe-area-inset-bottom, 0px), var(--safe-area-inset-bottom, 0px));

    /* Older names used across the components. */
    --ink: var(--text);
    --ink-soft: var(--text-2);
    --paper: var(--surface);
    --paper-tint: var(--surface-2);
    --sky-1: var(--surface-2);
    --sky-2: var(--surface-3);
    --sky-3: var(--surface);
    --line: rgba(255, 255, 255, 0.09);
    --green: var(--ok);
    --accent-deep: color-mix(in srgb, var(--accent) 78%, black);
    --fr: var(--font);
    --ns: var(--font);
    --mn: var(--font);
    --shadow: none;
    --shadow-sm: none;
  }
`;

/** Building blocks shared by every screen. */
export const sharedStyles = css`
  :host {
    font-family: var(--font);
    color: var(--text);
    -webkit-tap-highlight-color: transparent;
  }
  * {
    box-sizing: border-box;
  }
  button,
  input,
  select,
  textarea {
    font-family: var(--font);
  }
  h2,
  h3,
  h4 {
    margin: 0;
    font-weight: 600;
    letter-spacing: -0.01em;
  }
  p {
    margin: 0;
  }

  .muted {
    color: var(--muted);
    font-size: 0.9rem;
    line-height: 1.5;
    margin: 0;
  }
  .small {
    font-size: 0.8rem;
  }
  .error {
    margin: 0;
    color: var(--err);
    font-size: 0.9rem;
    line-height: 1.45;
  }
  .ok-text {
    color: var(--ok);
  }
  .notice {
    margin: 0;
    padding: 12px 14px;
    border-radius: var(--radius-sm);
    background: var(--surface-2);
    color: var(--text-2);
    font-size: 0.86rem;
    line-height: 1.45;
  }
  .notice.accent {
    background: var(--accent-soft);
    color: var(--text);
  }

  .card {
    display: flex;
    flex-direction: column;
    gap: 12px;
    padding: 16px;
    border-radius: var(--radius-lg);
    background: var(--surface);
  }
  .section-title {
    margin: 6px 4px 0;
    color: var(--muted);
    font-size: 0.75rem;
    font-weight: 600;
    letter-spacing: 0.06em;
    text-transform: uppercase;
  }

  /* Buttons: filled accent (main action), tonal (secondary), quiet. */
  .btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    min-height: 46px;
    padding: 0 18px;
    border: 0;
    border-radius: var(--radius-sm);
    background: var(--surface-3);
    color: var(--text);
    font-size: 0.95rem;
    font-weight: 600;
    text-align: center;
    text-decoration: none;
    cursor: pointer;
    transition:
      background 0.15s ease,
      transform 0.1s ease,
      opacity 0.15s ease;
  }
  .btn:active:not(:disabled) {
    transform: scale(0.98);
  }
  .btn:disabled {
    opacity: 0.45;
    cursor: default;
  }
  .btn.primary {
    background: var(--accent);
    color: var(--on-accent);
  }
  .btn.quiet {
    background: transparent;
    color: var(--accent-text);
  }
  .btn.danger {
    background: color-mix(in srgb, var(--err) 16%, transparent);
    color: var(--err);
  }
  .btn.small {
    min-height: 36px;
    padding: 0 14px;
    font-size: 0.85rem;
  }
  .btn.block {
    width: 100%;
  }
  .btn svg {
    flex: 0 0 auto;
  }
  .row {
    display: flex;
    gap: 8px;
    flex-wrap: wrap;
  }
  .row > .btn {
    flex: 1 1 0;
    min-width: 0;
  }

  /* Settings lists: rows on one surface, separated by hairlines. */
  .list {
    display: flex;
    flex-direction: column;
    border-radius: var(--radius-lg);
    background: var(--surface);
    overflow: hidden;
  }
  .item {
    display: flex;
    align-items: center;
    gap: 12px;
    min-height: 56px;
    padding: 10px 16px;
    border: 0;
    background: transparent;
    color: var(--text);
    font-size: 0.95rem;
    text-align: left;
    width: 100%;
  }
  .item + .item {
    border-top: 1px solid var(--line);
  }
  .item.stack {
    flex-direction: column;
    align-items: stretch;
    gap: 10px;
    padding: 14px 16px;
  }
  .item .label {
    flex: 1 1 auto;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .item .label small {
    color: var(--muted);
    font-size: 0.8rem;
    line-height: 1.4;
  }
  .item .value {
    flex: 0 0 auto;
    color: var(--muted);
    font-size: 0.88rem;
    font-variant-numeric: tabular-nums;
  }
  .item .head {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 10px;
  }
  button.item {
    cursor: pointer;
  }
  button.item:active {
    background: var(--surface-2);
  }

  /* Switch */
  .switch {
    appearance: none;
    -webkit-appearance: none;
    flex: 0 0 auto;
    width: 48px;
    height: 28px;
    margin: 0;
    border-radius: 999px;
    background: var(--surface-3);
    position: relative;
    cursor: pointer;
    transition: background 0.15s ease;
  }
  .switch::before {
    content: "";
    position: absolute;
    top: 3px;
    left: 3px;
    width: 22px;
    height: 22px;
    border-radius: 50%;
    background: #fff;
    transition: transform 0.15s ease;
  }
  .switch:checked {
    background: var(--accent);
  }
  .switch:checked::before {
    transform: translateX(20px);
  }

  /* Segmented control, like the reader's chips */
  .seg {
    display: flex;
    gap: 4px;
    padding: 4px;
    border-radius: var(--radius-sm);
    background: var(--surface-2);
    overflow-x: auto;
    scrollbar-width: none;
  }
  .seg::-webkit-scrollbar {
    display: none;
  }
  .seg button {
    flex: 1 1 0;
    min-width: 44px;
    min-height: 38px;
    padding: 0 10px;
    border: 0;
    border-radius: 8px;
    background: transparent;
    color: var(--text-2);
    font-size: 0.86rem;
    font-weight: 600;
    white-space: nowrap;
    cursor: pointer;
  }
  .seg button.on {
    background: var(--accent);
    color: var(--on-accent);
  }

  /* Range slider */
  input[type="range"] {
    -webkit-appearance: none;
    appearance: none;
    width: 100%;
    height: 28px;
    margin: 0;
    background: transparent;
  }
  input[type="range"]::-webkit-slider-runnable-track {
    height: 6px;
    border-radius: 3px;
    background: linear-gradient(to right, var(--accent) var(--fill, 50%), var(--surface-3) var(--fill, 50%));
  }
  input[type="range"]::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 22px;
    height: 22px;
    margin-top: -8px;
    border-radius: 50%;
    background: #fff;
    border: 0;
  }
  input[type="range"]::-moz-range-track {
    height: 6px;
    border-radius: 3px;
    background: var(--surface-3);
  }
  input[type="range"]::-moz-range-progress {
    height: 6px;
    border-radius: 3px;
    background: var(--accent);
  }
  input[type="range"]::-moz-range-thumb {
    width: 22px;
    height: 22px;
    border-radius: 50%;
    background: #fff;
    border: 0;
  }

  /* Text fields */
  .field {
    display: flex;
    flex-direction: column;
    gap: 6px;
    color: var(--muted);
    font-size: 0.8rem;
    font-weight: 600;
  }
  .input,
  select.input {
    width: 100%;
    min-height: 46px;
    padding: 0 14px;
    border: 1px solid transparent;
    border-radius: var(--radius-sm);
    background: var(--surface-2);
    color: var(--text);
    font-size: 1rem;
    font-weight: 400;
  }
  .input:focus {
    outline: none;
    border-color: var(--accent);
  }
  .input::placeholder {
    color: var(--muted);
  }

  /* Thin progress bar */
  .progress {
    height: 6px;
    border-radius: 3px;
    background: var(--surface-3);
    overflow: hidden;
  }
  .progress span {
    display: block;
    height: 100%;
    border-radius: 3px;
    background: var(--accent);
    transition: width 0.2s ease;
  }

  /* Full-screen sheet (editors, dialogs) */
  .sheet-backdrop {
    position: fixed;
    inset: 0;
    z-index: 50;
    display: flex;
    align-items: flex-end;
    justify-content: center;
    background: rgba(0, 0, 0, 0.6);
  }
  .sheet {
    width: min(640px, 100%);
    max-height: calc(100% - var(--safe-top) - 8px);
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 12px;
    padding: 16px 16px calc(16px + var(--safe-bottom));
    border-radius: var(--radius-lg) var(--radius-lg) 0 0;
    background: var(--bg);
    color: var(--text);
  }
  .sheet.full {
    height: 100%;
    max-height: none;
    border-radius: 0;
    padding-top: calc(12px + var(--safe-top));
  }
  .sheet-head {
    display: flex;
    align-items: center;
    gap: 12px;
  }
  .sheet-head .titles {
    flex: 1 1 auto;
    min-width: 0;
    display: flex;
    flex-direction: column;
  }
  .sheet-head small {
    color: var(--muted);
    font-size: 0.78rem;
    font-weight: 600;
  }
  .sheet-head strong {
    font-size: 1.1rem;
    font-weight: 600;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .icon-btn {
    flex: 0 0 auto;
    width: 40px;
    height: 40px;
    display: grid;
    place-items: center;
    border: 0;
    border-radius: 50%;
    background: var(--surface-2);
    color: var(--text);
    cursor: pointer;
  }
  .icon-btn:active {
    background: var(--surface-3);
  }
`;

/** Range input fill: set as style on the input (WebKit has no ::progress). */
export function rangeFill(value: number, min: number, max: number): string {
  const percent = max > min ? ((value - min) / (max - min)) * 100 : 0;
  return `--fill:${Math.max(0, Math.min(100, percent))}%`;
}

/** RGB565 (the reader's color format) -> CSS. */
export function rgb565(value: number): string {
  const r = Math.round((((value >> 11) & 0x1f) * 255) / 31);
  const g = Math.round((((value >> 5) & 0x3f) * 255) / 63);
  const b = Math.round(((value & 0x1f) * 255) / 31);
  return `rgb(${r}, ${g}, ${b})`;
}
