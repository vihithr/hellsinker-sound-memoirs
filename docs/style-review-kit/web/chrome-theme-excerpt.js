// ---- title / chrome -------------------------------------------------------
// Side-rail / help chrome follows the active theme (values from palettes.json).
const CHROME=[
  {bg:"rgba(20,28,26,.88)",border:"#84e6e0",btn:"rgba(36,48,44,.85)",hover:"rgba(60,80,72,.95)",ink:"#e8f2ee",accent:"#e87828",halo:"rgba(132,230,224,.10)"}, // ORIGINAL
  {bg:"rgba(10,13,18,.92)",border:"#3cdcd2",btn:"rgba(24,28,34,.9)",hover:"rgba(44,52,64,.95)",ink:"#dce1e6",accent:"#ff783c",halo:"rgba(60,220,210,.08)"},  // DARK
  {bg:"rgba(8,10,18,.92)",border:"#00ffdc",btn:"rgba(14,18,30,.9)",hover:"rgba(30,26,54,.95)",ink:"#e6f0ff",accent:"#ff3cb4",halo:"rgba(255,60,180,.10)"},  // NEON
  {bg:"rgba(238,232,218,.94)",border:"#5a6e64",btn:"rgba(222,216,202,.9)",hover:"rgba(200,192,172,.95)",ink:"#32302c",accent:"#aa3c1e",halo:"rgba(246,242,232,.12)"}, // PAPER
  {bg:"rgba(16,32,50,.92)",border:"#88c8d8",btn:"rgba(25,53,79,.9)",hover:"rgba(42,74,104,.95)",ink:"#e6eff1",accent:"#f2c184",halo:"rgba(136,200,216,.10)"}, // BLUEPRINT
];
function applyChrome(){
  const c=CHROME[theme]||CHROME[0],r=document.documentElement.style;
  r.setProperty("--chrome-bg",c.bg);r.setProperty("--chrome-border",c.border);
  r.setProperty("--chrome-btn",c.btn);r.setProperty("--chrome-btn-hover",c.hover);
  r.setProperty("--chrome-ink",c.ink);r.setProperty("--chrome-accent",c.accent);
  r.setProperty("--halo",c.halo);
}
function refreshTitle(){
  const t=THEMES[theme];
  document.title=`SOUND MEMOIRS  [VOL ${volume}] [${t}]`+(loading?"  [LOADING]":"");
  applyChrome();
}
function markRailActivity
