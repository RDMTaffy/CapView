#!/usr/bin/env python3
"""Draws the DMG window background (drag-to-install arrow + first-open steps).

Writes Resources/dmg/background.png (720x480) and background@2x.png; make-dmg.sh
combines them into a Retina TIFF. Needs Pillow (python3 -m pip install pillow).
Icon positions must match make-dmg.sh: CapView at (180, 128), Applications at (540, 128).
Everything sits in the top 450 pt so a Finder tab bar (about 30 pt) cannot cut it off.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT = "/System/Library/Fonts/AppleSDGothicNeo.ttc"
WEIGHT = {"regular": 0, "medium": 2, "semibold": 4, "bold": 6}

INK = (29, 29, 31)
SECONDARY = (110, 110, 115)
BLUE = (10, 132, 255)
RING = (255, 59, 48)
CARD = (255, 255, 255)
CARD_EDGE = (222, 224, 229)
WINDOW = (242, 242, 245)
WINDOW_EDGE = (214, 214, 220)
BAR = (199, 199, 204)

W, H = 720, 480


def draw(scale: int) -> Image.Image:
    s = scale
    img = Image.new("RGB", (W * s, H * s))
    # Soft vertical gradient.
    top, bottom = (247, 248, 250), (236, 238, 242)
    for y in range(H * s):
        t = y / (H * s - 1)
        c = tuple(round(top[i] * (1 - t) + bottom[i] * t) for i in range(3))
        ImageDraw.Draw(img).line([(0, y), (W * s, y)], fill=c)
    d = ImageDraw.Draw(img)

    def font(size, weight="regular"):
        return ImageFont.truetype(FONT, round(size * s), index=WEIGHT[weight])

    def text(x, y, value, size, weight="regular", fill=INK, anchor="la"):
        d.text((x * s, y * s), value, font=font(size, weight), fill=fill, anchor=anchor)

    def rect(x0, y0, x1, y1, radius, fill=None, outline=None, width=1):
        d.rounded_rectangle([x0 * s, y0 * s, x1 * s, y1 * s], radius=radius * s,
                            fill=fill, outline=outline, width=round(width * s))

    def bars(x, y, widths, gap=7, height=3):
        for i, w in enumerate(widths):
            rect(x, y + i * gap, x + w, y + i * gap + height, 1.5, fill=BAR)

    def button(x0, y0, x1, y1, label, primary=False, ring=False):
        if ring:
            rect(x0 - 3.5, y0 - 3.5, x1 + 3.5, y1 + 3.5, 9, outline=RING, width=2)
        rect(x0, y0, x1, y1, 6, fill=BLUE if primary else (227, 227, 232))
        text((x0 + x1) / 2, (y0 + y1) / 2 + 0.5, label, 7.5, "semibold",
             fill=(255, 255, 255) if primary else INK, anchor="mm")

    # 1. Drag to install.
    text(W / 2, 30, "CapView를 응용 프로그램 폴더로 끌어다 놓으세요", 18, "semibold", anchor="mm")
    y = 128
    d.line([(268 * s, y * s), (438 * s, y * s)], fill=BLUE, width=round(7 * s))
    d.polygon([(456 * s, y * s), (434 * s, (y - 14) * s), (434 * s, (y + 14) * s)], fill=BLUE)
    d.ellipse([(264.5 * s), (y - 3.5) * s, (271.5 * s), (y + 3.5) * s], fill=BLUE)

    # 2. First open, three steps.
    d.line([(40 * s, 232 * s), (680 * s, 232 * s)], fill=(216, 218, 224), width=s)
    text(40, 257, "처음 열 때 한 번만 해 주세요", 15, "semibold", anchor="ls")
    text(250, 257, "Apple 공증을 받지 않은 개인 앱이라서 필요해요", 11, fill=SECONDARY, anchor="ls")

    steps = [
        ("CapView를 열고 [완료]", "경고창이 뜨면 '완료'를 누르세요"),
        ("설정에서 [그래도 열기]", "시스템 설정 › 개인정보 보호 및 보안 › 맨 아래"),
        ("다시 뜨는 창에서 [열기]", "Mac 암호 입력 후 · 다음부터는 바로 열려요"),
    ]
    for i, (title, sub) in enumerate(steps):
        x, y = 40 + i * 220, 270
        rect(x, y, x + 200, y + 165, 12, fill=CARD, outline=CARD_EDGE)
        d.ellipse([(x + 12) * s, (y + 11) * s, (x + 32) * s, (y + 31) * s], fill=BLUE)
        text(x + 22, y + 21.5, str(i + 1), 11, "bold", fill=(255, 255, 255), anchor="mm")
        text(x + 40, y + 21.5, title, 12.5, "semibold", anchor="lm")
        text(x + 14, y + 44, sub, 9.5, fill=SECONDARY, anchor="lm")

        if i == 0:  # The "not opened" alert: [휴지통으로 이동] / [완료].
            rect(x + 45, y + 60, x + 155, y + 156, 10, fill=WINDOW, outline=WINDOW_EDGE)
            rect(x + 91, y + 67, x + 109, y + 85, 4, fill=BAR)
            d.polygon([((x + 103) * s, (y + 76) * s), ((x + 97) * s, (y + 87) * s),
                       ((x + 109) * s, (y + 87) * s)], fill=(255, 196, 0))
            bars(x + 65, y + 93, [70, 56])
            button(x + 55, y + 112, x + 145, y + 126, "휴지통으로 이동", primary=True)
            button(x + 55, y + 133, x + 145, y + 147, "완료", ring=True)
        elif i == 1:  # System Settings › Privacy & Security, [그래도 열기] at the bottom.
            rect(x + 14, y + 60, x + 186, y + 156, 10, fill=WINDOW, outline=WINDOW_EDGE)
            d.rounded_rectangle([(x + 14) * s, (y + 60) * s, (x + 66) * s, (y + 156) * s],
                                radius=10 * s, fill=(232, 232, 237), corners=(True, False, False, True))
            bars(x + 21, y + 71, [34, 28, 38], gap=9)
            rect(x + 18, y + 99, x + 62, y + 111, 5, fill=BLUE)
            text(x + 40, y + 105.5, "개인정보 보호", 6.5, "semibold", fill=(255, 255, 255), anchor="mm")
            bars(x + 21, y + 119, [30, 36], gap=9)
            bars(x + 76, y + 70, [96, 80, 90], gap=8)
            text(x + 76, y + 108, "'CapView'이(가) 차단됨", 7, fill=SECONDARY, anchor="lm")
            button(x + 112, y + 128, x + 178, y + 143, "그래도 열기", ring=True)
        else:  # The follow-up alert with [열기].
            rect(x + 45, y + 60, x + 155, y + 156, 10, fill=WINDOW, outline=WINDOW_EDGE)
            rect(x + 91, y + 67, x + 109, y + 85, 4, fill=BAR)
            text(x + 100, y + 99, "'CapView'을(를) 열까요?", 7.5, "semibold", anchor="mm")
            bars(x + 65, y + 106, [70])
            button(x + 55, y + 117, x + 145, y + 131, "열기", primary=True, ring=True)
            button(x + 55, y + 136, x + 145, y + 150, "취소")
    return img


def main():
    out = Path(__file__).resolve().parent.parent / "Resources" / "dmg"
    out.mkdir(parents=True, exist_ok=True)
    draw(1).save(out / "background.png")
    draw(2).save(out / "background@2x.png")
    print(f"Wrote {out / 'background.png'} and @2x")


if __name__ == "__main__":
    main()
