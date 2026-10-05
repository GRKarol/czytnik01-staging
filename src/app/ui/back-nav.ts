/**
 * The phone's Back button: the innermost open screen closes first.
 *
 * Panels register a handler while they are on screen; it returns true when
 * it closed something of its own (a sub-page, an editor, a confirm row).
 * Handlers registered later sit deeper in the UI, so they run first. When no
 * handler takes the press, the shell steps back through the tabs and, on
 * Home, asks before leaving the app (app.element.ts).
 */

type BackHandler = () => boolean;

const handlers: BackHandler[] = [];

export function onBack(handler: BackHandler): () => void {
  handlers.push(handler);
  return () => {
    const index = handlers.indexOf(handler);
    if (index >= 0) handlers.splice(index, 1);
  };
}

/** True when an open screen took the press. */
export function runBackHandlers(): boolean {
  for (let i = handlers.length - 1; i >= 0; --i) {
    if (handlers[i]()) return true;
  }
  return false;
}
