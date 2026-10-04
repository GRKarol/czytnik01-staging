import { svg, type SVGTemplateResult } from "lit";

/** Line icons in the reader's style: 24 px grid, round caps, 1.8 stroke. */
function icon(paths: SVGTemplateResult, size: number): SVGTemplateResult {
  return svg`<svg width=${size} height=${size} viewBox="0 0 24 24" fill="none" stroke="currentColor"
    stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${paths}</svg>`;
}

export const icons = {
  home: (s = 24) => icon(svg`<path d="M3 11l9-8 9 8v9a2 2 0 0 1-2 2h-4v-7h-6v7H5a2 2 0 0 1-2-2z"/>`, s),
  books: (s = 24) =>
    icon(svg`<path d="M4 4h3v16H4zM9 4h3v16H9z"/><path d="M14.5 5.2l2.9-.8 3.6 15-2.9.8z"/>`, s),
  convert: (s = 24) => icon(svg`<path d="M5 8h12l-3-3"/><path d="M19 16H7l3 3"/>`, s),
  plugins: (s = 24) =>
    icon(svg`<rect x="4" y="4" width="7" height="7" rx="1.5"/><rect x="13" y="4" width="7" height="7" rx="1.5"/>
      <rect x="4" y="13" width="7" height="7" rx="1.5"/><rect x="13" y="13" width="7" height="7" rx="3.5"/>`, s),
  more: (s = 24) =>
    icon(svg`<path d="M4 7h10M18 7h2M4 17h2M10 17h10"/><circle cx="16" cy="7" r="2"/><circle cx="8" cy="17" r="2"/>`, s),
  wifi: (s = 24) =>
    icon(svg`<path d="M2 8.5a17 17 0 0 1 20 0"/><path d="M5 12a13 13 0 0 1 14 0"/><path d="M8.5 15.5a8 8 0 0 1 7 0"/>
      <circle cx="12" cy="19" r="1.2" fill="currentColor"/>`, s),
  bluetooth: (s = 24) => icon(svg`<path d="M7 7l10 10-5 4V3l5 4L7 17"/>`, s),
  usb: (s = 24) =>
    icon(svg`<circle cx="12" cy="4" r="1.5"/><path d="M12 5.5V20"/><path d="M12 14l-4-4V8"/><path d="M12 12l4-2V8"/>
      <rect x="9" y="20" width="6" height="2" rx="1"/>`, s),
  update: (s = 24) => icon(svg`<path d="M21 12a9 9 0 1 1-3-6.7"/><path d="M21 4v5h-5"/>`, s),
  chevronRight: (s = 20) => icon(svg`<path d="M9 6l6 6-6 6"/>`, s),
  chevronLeft: (s = 20) => icon(svg`<path d="M15 6l-6 6 6 6"/>`, s),
  close: (s = 20) => icon(svg`<path d="M6 6l12 12M18 6L6 18"/>`, s),
  play: (s = 20) => svg`<svg width=${s} height=${s} viewBox="0 0 24 24" aria-hidden="true"><path d="M8 5v14l11-7z" fill="currentColor"/></svg>`,
  plus: (s = 20) => icon(svg`<path d="M12 5v14M5 12h14"/>`, s),
  upload: (s = 20) => icon(svg`<path d="M12 16V4M7 9l5-5 5 5"/><path d="M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3"/>`, s),
  download: (s = 20) => icon(svg`<path d="M12 4v12M7 11l5 5 5-5"/><path d="M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3"/>`, s),
  send: (s = 20) => icon(svg`<path d="M4 12l16-8-6 16-2.5-6.5z"/><path d="M11.5 13.5L20 4"/>`, s),
  image: (s = 20) =>
    icon(svg`<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="10" r="2"/><path d="M21 16l-5-5-9 9"/>`, s),
  chapters: (s = 20) => icon(svg`<path d="M9 6h11M9 12h11M9 18h11"/><circle cx="4.5" cy="6" r="1"/><circle cx="4.5" cy="12" r="1"/><circle cx="4.5" cy="18" r="1"/>`, s),
  restart: (s = 20) => icon(svg`<path d="M4 12a8 8 0 1 0 2.3-5.7"/><path d="M4 4v4h4"/>`, s),
  trash: (s = 20) =>
    icon(svg`<path d="M4 7h16M10 11v6M14 11v6"/><path d="M6 7l1 13h10l1-13"/><path d="M9 7V4h6v3"/>`, s),
  star: (s = 20, filled = false) =>
    svg`<svg width=${s} height=${s} viewBox="0 0 24 24" fill=${filled ? "currentColor" : "none"} stroke="currentColor"
      stroke-width="1.8" stroke-linejoin="round" aria-hidden="true"><path d="M12 3.5l2.6 5.4 5.9.8-4.3 4.1 1 5.8L12 16.8l-5.2 2.8 1-5.8-4.3-4.1 5.9-.8z"/></svg>`,
  check: (s = 20) => icon(svg`<path d="M5 12.5l4.5 4.5L19 7"/>`, s),
  battery: (s = 20) => icon(svg`<rect x="2" y="7" width="17" height="10" rx="2"/><path d="M22 10v4"/><path d="M5 10h8v4H5z" fill="currentColor" stroke="none"/>`, s),
  sd: (s = 20) => icon(svg`<path d="M7 3h8l4 4v13a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"/><path d="M10 3v4M13 3v4"/>`, s),
  chip: (s = 20) => icon(svg`<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/>`, s),
  globe: (s = 20) => icon(svg`<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18a14 14 0 0 1 0-18"/>`, s),
  sun: (s = 20) =>
    icon(svg`<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4 12H2M22 12h-2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>`, s),
  type: (s = 20) => icon(svg`<path d="M5 6V4h14v2M12 4v16M9 20h6"/>`, s),
  palette: (s = 20) =>
    icon(svg`<path d="M12 3a9 9 0 1 0 0 18c1.1 0 1.6-.9 1.2-1.8l-.4-.9c-.5-1.1.3-2.3 1.5-2.3H17a4 4 0 0 0 4-4c0-5-4-9-9-9z"/>
      <circle cx="7.5" cy="11" r="1.2" fill="currentColor"/><circle cx="10.5" cy="7" r="1.2" fill="currentColor"/><circle cx="15" cy="7.5" r="1.2" fill="currentColor"/>`, s),
  gauge: (s = 20) => icon(svg`<path d="M12 13l4-4"/><path d="M3.5 17a9 9 0 1 1 17 0"/>`, s),
  moon: (s = 20) => icon(svg`<path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z"/>`, s),
  help: (s = 20) => icon(svg`<circle cx="12" cy="12" r="9"/><path d="M9.5 9.5a2.5 2.5 0 0 1 4.8 1c0 1.7-2.3 2-2.3 3.5"/><circle cx="12" cy="17" r=".6" fill="currentColor"/>`, s),
  code: (s = 20) => icon(svg`<path d="M8 5L3 12l5 7M16 5l5 7-5 7"/>`, s),
  search: (s = 20) => icon(svg`<circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/>`, s),
  file: (s = 20) => icon(svg`<path d="M6 3h9l4 4v14H6z"/><path d="M14 3v5h5"/>`, s),
  phone: (s = 20) => icon(svg`<rect x="7" y="2" width="10" height="20" rx="2"/><path d="M11 18h2"/>`, s),
  layout: (s = 20) => icon(svg`<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M9 4v16"/>`, s),
  power: (s = 20) => icon(svg`<path d="M12 3v9"/><path d="M6.3 6.3a8 8 0 1 0 11.4 0"/>`, s),
  hand: (s = 20) => icon(svg`<path d="M8 13V5.5a1.5 1.5 0 0 1 3 0V12M11 11V4.5a1.5 1.5 0 0 1 3 0V11M14 11V6.5a1.5 1.5 0 0 1 3 0V14a7 7 0 0 1-7 7h-.5A6.5 6.5 0 0 1 4 15.5V13a1.5 1.5 0 0 1 3 0"/>`, s),
};
