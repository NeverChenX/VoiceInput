"""Generate VoiceInput project icons (flat minimal microphone)."""
from __future__ import annotations

from pathlib import Path
from PIL import Image, ImageDraw

ASSETS = Path(__file__).parent
TRAY_DIR = ASSETS / "tray"
TRAY_DIR.mkdir(exist_ok=True)

SUPER = 4096          # supersample for clean anti-aliased edges
TARGET = 1024         # master output size

BRAND = (79, 70, 229, 255)        # indigo-600 — project icon
TRAY_IDLE = (113, 113, 122, 255)  # zinc-500 — 待机
TRAY_REC = (239, 68, 68, 255)     # red-500 — 录音
TRAY_RECO = (59, 130, 246, 255)   # blue-500 — 识别
WHITE = (255, 255, 255, 255)


def draw_microphone(size: int, bg: tuple[int, int, int, int] | None,
                    fg: tuple[int, int, int, int] = WHITE,
                    bg_radius_pct: float = 0.22) -> Image.Image:
    """Draw the icon at supersampled `size`. If bg is None, transparent background."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    if bg is not None:
        r = int(size * bg_radius_pct)
        d.rounded_rectangle((0, 0, size - 1, size - 1), radius=r, fill=bg)

    cx = size / 2

    # Capsule (stadium) — top of the mic
    cap_w = size * 0.30
    cap_h = size * 0.46
    cap_top = size * 0.16
    cap_left = cx - cap_w / 2
    cap_right = cx + cap_w / 2
    cap_bot = cap_top + cap_h
    d.rounded_rectangle((cap_left, cap_top, cap_right, cap_bot),
                        radius=cap_w / 2, fill=fg)

    # U-shape cradle: drawn as the lower half of a thick ring.
    # Outer circle bbox is centered horizontally; vertically it sits so that
    # the upper half passes BEHIND the capsule and only the lower arc shows.
    arc_outer_w = size * 0.56
    arc_thickness = size * 0.055
    arc_cx = cx
    arc_cy = size * 0.58           # vertical center of the ring
    bbox_outer = (arc_cx - arc_outer_w / 2,
                  arc_cy - arc_outer_w / 2,
                  arc_cx + arc_outer_w / 2,
                  arc_cy + arc_outer_w / 2)
    # PIL angles: 0 = 3 o'clock, sweeping clockwise. 0..180 = right -> bottom -> left.
    d.arc(bbox_outer, start=0, end=180, fill=fg, width=int(arc_thickness))

    # Stem from arc bottom down to base
    stem_w = size * 0.045
    stem_top = arc_cy + arc_outer_w / 2 - arc_thickness * 0.6  # slight overlap with arc
    stem_bot = size * 0.84
    d.rounded_rectangle((cx - stem_w / 2, stem_top, cx + stem_w / 2, stem_bot),
                        radius=stem_w / 2, fill=fg)

    # Base (foot)
    base_w = size * 0.30
    base_h = size * 0.05
    base_top = stem_bot - base_h * 0.2  # overlap stem so they fuse
    d.rounded_rectangle((cx - base_w / 2, base_top, cx + base_w / 2, base_top + base_h),
                        radius=base_h / 2, fill=fg)

    return img


def render(bg, fg=WHITE, transparent_bg=False, target=TARGET) -> Image.Image:
    super_img = draw_microphone(SUPER, None if transparent_bg else bg, fg=fg)
    return super_img.resize((target, target), Image.LANCZOS)


def main() -> None:
    # ---- Master project icon ----
    master = render(BRAND, target=1024)
    master.save(ASSETS / "icon.png", optimize=True)
    master.save(ASSETS / "icon@1024.png", optimize=True)
    render(BRAND, target=512).save(ASSETS / "icon@512.png", optimize=True)
    render(BRAND, target=256).save(ASSETS / "icon@256.png", optimize=True)

    # ---- Windows .ico (embedded multi-size) ----
    # Pillow's ICO writer drops sizes larger than the base image, so save
    # in descending order: largest first as base, smaller ones appended.
    ico_sizes = [256, 128, 64, 48, 32, 24, 16]
    ico_imgs = [render(BRAND, target=s) for s in ico_sizes]
    ico_imgs[0].save(
        ASSETS / "icon.ico",
        format="ICO",
        sizes=[(s, s) for s in ico_sizes],
        append_images=ico_imgs[1:],
    )

    # ---- macOS .icns ----
    # Pillow's ICNS writer requires specific sizes. Provide the standard set.
    icns_sizes = [16, 32, 64, 128, 256, 512, 1024]
    base_for_icns = render(BRAND, target=1024)
    base_for_icns.save(ASSETS / "icon.icns", format="ICNS",
                       sizes=[(s, s) for s in icns_sizes])

    # ---- Tray three-state variants (transparent bg, colored mic) ----
    tray_states = {"idle": TRAY_IDLE, "recording": TRAY_REC, "recognizing": TRAY_RECO}
    for name, color in tray_states.items():
        for px in (16, 32, 64):
            img = render(None, fg=color, transparent_bg=True, target=px)
            img.save(TRAY_DIR / f"tray-{name}-{px}.png", optimize=True)
        # also a multi-size .ico for convenience (Windows tray prefers ICO)
        tray_sizes = [64, 48, 32, 24, 16]
        tray_imgs = [render(None, fg=color, transparent_bg=True, target=s)
                     for s in tray_sizes]
        tray_imgs[0].save(
            TRAY_DIR / f"tray-{name}.ico",
            format="ICO",
            sizes=[(s, s) for s in tray_sizes],
            append_images=tray_imgs[1:],
        )

    print("Wrote:")
    for p in sorted(ASSETS.rglob("*")):
        if p.is_file() and p.name != "make_icons.py":
            print(" ", p.relative_to(ASSETS))


if __name__ == "__main__":
    main()
