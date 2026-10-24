/**
 * Browser-only preferences (the game's own settings live in its settings.dat). Kept in this
 * site's localStorage; every access tolerates storage being unavailable.
 */
export interface Prefs {
  /** KeyboardEvent.code that the game also receives as Escape. */
  pauseAlias: string;
  /** Resolution follows the screen at every start (else settings.dat's VM_WIDTH/HEIGHT stay). */
  autoResolution: boolean;
}

const KEY = "bella.prefs";
export const DEFAULT_PREFS: Prefs = { pauseAlias: "Backquote", autoResolution: true };

export function loadPrefs(): Prefs {
  try {
    return { ...DEFAULT_PREFS, ...(JSON.parse(localStorage.getItem(KEY) ?? "{}") as Partial<Prefs>) };
  } catch {
    return { ...DEFAULT_PREFS };
  }
}

export function savePrefs(p: Prefs): void {
  try {
    localStorage.setItem(KEY, JSON.stringify(p));
  } catch {
    // Private windows may refuse storage; the defaults apply next time.
  }
}
