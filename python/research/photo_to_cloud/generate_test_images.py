"""Create a small synthetic multiview scene for reconstruction smoke tests."""

import argparse
from pathlib import Path
import random

from PIL import Image, ImageDraw


def generate(output: Path) -> None:
    randomizer = random.Random(9135)
    output.mkdir(parents=True, exist_ok=False)
    width, height, focal = 960, 720, 820
    points = []
    for row in range(9):
        for column in range(12):
            depth = randomizer.uniform(4.0, 9.0)
            x = (column - 5.5) * 67 * depth / focal
            y = (row - 4) * 67 * depth / focal
            tag = Image.new("RGB", (43, 43), (210, 210, 210))
            draw = ImageDraw.Draw(tag)
            draw.rectangle((0, 0, 42, 42), outline=(15, 15, 15), width=2)
            for _ in range(13):
                coords = tuple(randomizer.randrange(4, 39) for _ in range(4))
                color = tuple(randomizer.randrange(10, 210) for _ in range(3))
                draw.line(coords, fill=color, width=randomizer.choice((1, 2, 3)))
            for _ in range(4):
                px, py = randomizer.randrange(7, 33), randomizer.randrange(7, 33)
                radius = randomizer.randrange(2, 6)
                color = tuple(randomizer.randrange(20, 220) for _ in range(3))
                draw.ellipse((px-radius, py-radius, px+radius, py+radius), fill=color)
            points.append((x, y, depth, tag))
    for index in range(7):
        canvas = Image.new("RGB", (width, height), (238, 238, 238))
        camera_x = (index - 3) * 0.20
        for x, y, depth, tag in sorted(points, key=lambda point: point[2], reverse=True):
            u = int(width / 2 + focal * (x - camera_x) / depth)
            v = int(height / 2 + focal * y / depth)
            canvas.paste(tag, (u - 21, v - 21))
        canvas.save(output / f"{index:03}.png")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    generate(parser.parse_args().output)
