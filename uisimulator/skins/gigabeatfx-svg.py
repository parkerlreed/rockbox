"""Writes gigabeatfx.svg, the Toshiba Gigabeat F/X face for the simulator:
the brushed aluminium front with the cross pad, and the side with Power,
Menu, volume and A. nanosvg draws no textures, so the brushing is generated
as faint streaks.

Usage: gigabeatfx-svg.py [out.svg], next to this script by default
"""
import os, random, sys
out = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "gigabeatfx.svg")
random.seed(7)
W, H = 401, 655
# brushed aluminium: faint vertical streaks of varying brightness
streaks = []
x = 16.0
while x < 321:
    wdt = random.uniform(0.4, 1.6)
    op = random.uniform(0.02, 0.09)
    col = "#ffffff" if random.random() < 0.5 else "#000000"
    streaks.append(f'<rect x="{x:.1f}" y="11" width="{wdt:.1f}" height="644" fill="{col}" fill-opacity="{op:.3f}"/>')
    x += wdt + random.uniform(0.2, 1.4)
streaks = "\n      ".join(streaks)
svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
  <!-- Toshiba Gigabeat F/X face for the simulator: the brushed aluminium
       front and, on the right, the side with Power, Menu, volume and A.
       Same geometry as UI-gigabeatfx.bmp: LCD at 48,60 (240x320). Written
       by uisimulator/skins/gigabeatfx-svg.py. Fixed colours. -->
  <!-- nanosvg only draws gradients in user space properly -->
  <defs>
    <linearGradient id="face" x1="0" y1="11" x2="0" y2="655" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#c3c6ca"/>
      <stop offset="1" stop-color="#a9adb2"/>
    </linearGradient>
    <linearGradient id="side" x1="338" y1="0" x2="392" y2="0" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#7c8086"/>
      <stop offset="0.5" stop-color="#9da1a7"/>
      <stop offset="1" stop-color="#787c82"/>
    </linearGradient>
    <linearGradient id="glass" x1="0" y1="52" x2="0" y2="389" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#1c1c20"/>
      <stop offset="1" stop-color="#050505"/>
    </linearGradient>
    <linearGradient id="pad" x1="0" y1="433" x2="0" y2="641" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#e2e0da"/>
      <stop offset="1" stop-color="#c8c5be"/>
    </linearGradient>
    <linearGradient id="key" x1="0" y1="160" x2="0" y2="550" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#c9ccd0"/>
      <stop offset="1" stop-color="#a6aaaf"/>
    </linearGradient>
  </defs>

  <!-- background, and the light edge between front and side -->
  <rect x="0" y="0" width="{W}" height="{H}" fill="#d9dadc"/>

  <g id="front">
    <rect x="16" y="11" width="305" height="660" rx="12" fill="url(#face)"/>
    <g id="brushing">
      {streaks}
    </g>
    <rect x="16.5" y="11.5" width="304" height="660" rx="11.5" fill="none"
          stroke="#000000" stroke-opacity="0.35" stroke-width="1"/>
    <rect x="18" y="13" width="301" height="660" rx="10" fill="none"
          stroke="#ffffff" stroke-opacity="0.45" stroke-width="1"/>
  </g>

  <g id="screen">
    <rect x="38" y="51" width="257" height="339" rx="5" fill="url(#glass)"/>
    <rect x="48" y="60" width="240" height="320" fill="#000000"/>
  </g>

  <g id="pad">
    <!-- recess around the cross, following its outline -->
    <path d="M168,433 a22,22 0 0 1 22,22 v60 h60 a22,22 0 0 1 0,44 h-60 v60 a22,22 0 0 1 -44,0 v-60 h-60 a22,22 0 0 1 0,-44 h60 v-60 a22,22 0 0 1 22,-22 z"
          fill="none" stroke="#000000" stroke-opacity="0.22" stroke-width="9" stroke-linejoin="round"/>
    <!-- the cross -->
    <path d="M168,433 a22,22 0 0 1 22,22 v60 h60 a22,22 0 0 1 0,44 h-60 v60 a22,22 0 0 1 -44,0 v-60 h-60 a22,22 0 0 1 0,-44 h60 v-60 a22,22 0 0 1 22,-22 z"
          fill="url(#pad)" stroke="#6f6d68" stroke-opacity="0.6" stroke-width="1"/>
    <circle cx="168" cy="537" r="14" fill="none" stroke="#000000" stroke-opacity="0.12" stroke-width="1"/>
  </g>

  <g id="side">
    <rect x="338" y="0" width="54" height="{H}" fill="url(#side)"/>
    <rect x="338.5" y="0" width="53" height="{H}" fill="none" stroke="#000000" stroke-opacity="0.3" stroke-width="1"/>
    <!-- Power -->
    <circle cx="365" cy="187" r="13" fill="url(#key)" stroke="#55585d" stroke-width="1"/>
    <path d="M361.5,182.5 a6,6 0 1 0 7,0" fill="none" stroke="#3c3f44" stroke-width="1.4"/>
    <rect x="364.3" y="179" width="1.4" height="7" fill="#3c3f44"/>
    <!-- Menu -->
    <circle cx="365" cy="270" r="11" fill="url(#key)" stroke="#55585d" stroke-width="1"/>
    <rect x="360" y="266" width="10" height="1.3" fill="#3c3f44"/>
    <rect x="360" y="269.4" width="10" height="1.3" fill="#3c3f44"/>
    <rect x="360" y="272.8" width="10" height="1.3" fill="#3c3f44"/>
    <!-- volume rocker -->
    <rect x="351" y="324" width="28" height="130" rx="14" fill="url(#key)" stroke="#55585d" stroke-width="1"/>
    <rect x="360" y="342.4" width="10" height="1.6" fill="#3c3f44"/>
    <rect x="364.2" y="338.2" width="1.6" height="10" fill="#3c3f44"/>
    <rect x="360" y="434.4" width="10" height="1.6" fill="#3c3f44"/>
    <rect x="352" y="388.6" width="26" height="0.8" fill="#000000" fill-opacity="0.25"/>
    <!-- A -->
    <circle cx="365" cy="520" r="11" fill="url(#key)" stroke="#55585d" stroke-width="1"/>
    <path d="M361,525 l3.2,-10 h1.6 l3.2,10 h-1.7 l-0.8,-2.6 h-3 l-0.8,2.6 z M363.9,521 h2.2 l-1.1,-3.6 z" fill="#3c3f44"/>
  </g>
</svg>
'''
open(out, "w").write(svg)
