import argparse
import math
from pathlib import Path

import cv2
from PIL import Image


def rotate_frame(frame_bgr, angle: float):
    """按角度旋转 BGR 图像；90 的倍数自动扩展画布以防裁切。"""
    angle_mod = angle % 360
    if math.isclose(angle_mod, 0.0, abs_tol=1e-6):
        return frame_bgr
    if math.isclose(angle_mod, 90.0, abs_tol=1e-6):
        return cv2.rotate(frame_bgr, cv2.ROTATE_90_COUNTERCLOCKWISE)
    if math.isclose(angle_mod, 180.0, abs_tol=1e-6):
        return cv2.rotate(frame_bgr, cv2.ROTATE_180)
    if math.isclose(angle_mod, 270.0, abs_tol=1e-6):
        return cv2.rotate(frame_bgr, cv2.ROTATE_90_CLOCKWISE)

    h, w = frame_bgr.shape[:2]
    cx, cy = w / 2.0, h / 2.0
    rot_mat = cv2.getRotationMatrix2D((cx, cy), angle, 1.0)

    # 计算旋转后的外接矩形宽高，保证完整保留画面
    rad = math.radians(angle)
    cos_a = abs(math.cos(rad))
    sin_a = abs(math.sin(rad))
    new_w = int(round(w * cos_a + h * sin_a))
    new_h = int(round(w * sin_a + h * cos_a))

    rot_mat[0, 2] += (new_w / 2.0) - cx
    rot_mat[1, 2] += (new_h / 2.0) - cy

    return cv2.warpAffine(
        frame_bgr,
        rot_mat,
        (new_w, new_h),
        flags=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_CONSTANT,
        borderValue=(0, 0, 0),
    )


def video_to_gif(
    video_path: str,
    output_path: str,
    angle: float = 0.0,
    step: int = 1,
    fps: float | None = None,
    max_width: int | None = 640,
    start_sec: float = 0.0,
    end_sec: float | None = None,
    loop: int = 0,
):
    """
    从视频抽帧并旋转后生成 GIF。

    Args:
        video_path: 输入视频路径
        output_path: 输出 GIF 路径
        angle: 旋转角度（度），正值为逆时针，负值为顺时针
        step: 抽帧步长（每隔 step 帧取 1 帧，>=1）
        fps: GIF 播放帧率；若为 None，则按原视频帧率 / step 自动计算
        max_width: 输出最大宽度（像素），用于控制 GIF 体积；None 表示不缩放
        start_sec: 起始时间（秒）
        end_sec: 结束时间（秒），None 表示到视频末尾
        loop: 循环次数，0 表示无限循环
    """
    if step < 1:
        raise ValueError("step 必须 >= 1")

    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        raise FileNotFoundError(f"无法打开视频文件: {video_path}")

    src_fps = cap.get(cv2.CAP_PROP_FPS)
    if not src_fps or src_fps <= 0:
        src_fps = 25.0

    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    start_frame = max(0, int(start_sec * src_fps))
    end_frame = total_frames if end_sec is None else min(total_frames, int(end_sec * src_fps))

    if start_frame > 0:
        cap.set(cv2.CAP_PROP_POS_FRAMES, start_frame)

    pil_frames = []
    frame_idx = start_frame

    while frame_idx < end_frame:
        ret, frame = cap.read()
        if not ret:
            break

        if (frame_idx - start_frame) % step == 0:
            rotated = rotate_frame(frame, angle)

            if max_width is not None and rotated.shape[1] > max_width:
                scale = max_width / rotated.shape[1]
                new_size = (max_width, max(1, int(round(rotated.shape[0] * scale))))
                rotated = cv2.resize(rotated, new_size, interpolation=cv2.INTER_AREA)

            rgb = cv2.cvtColor(rotated, cv2.COLOR_BGR2RGB)
            pil_frames.append(Image.fromarray(rgb))

        frame_idx += 1

    cap.release()

    if not pil_frames:
        raise RuntimeError("没有提取到任何有效帧，请检查视频路径或时间范围。")

    out_fps = fps if (fps and fps > 0) else max(1.0, src_fps / step)
    duration_ms = max(10, int(round(1000.0 / out_fps)))

    out_file = Path(output_path)
    out_file.parent.mkdir(parents=True, exist_ok=True)

    pil_frames[0].save(
        out_file,
        save_all=True,
        append_images=pil_frames[1:],
        duration=duration_ms,
        loop=loop,
        optimize=False,
    )
    print(f"已生成 GIF: {out_file}（共 {len(pil_frames)} 帧，帧间隔 {duration_ms} ms，旋转 {angle}°）")


def main():
    parser = argparse.ArgumentParser(description="视频抽帧旋转生成 GIF")
    parser.add_argument("-input", default=r"E:\lfm\stereo-camera-iphone\docs\demo.mp4", help="输入视频文件路径")
    parser.add_argument("-o", "--output", default=r"E:\lfm\stereo-camera-iphone\docs\demo.gif", help="输出 GIF 路径（默认 output.gif）")
    parser.add_argument("-a", "--angle", type=float, default=90.0, help="旋转角度（度，正数逆时针，负数顺时针）")
    parser.add_argument("-s", "--step", type=int, default=5, help="抽帧步长（每隔 s 帧取 1 帧，默认 3）")
    parser.add_argument("--fps", type=float, default=None, help="GIF 帧率（默认按原视频帧率/步长自动计算）")
    parser.add_argument("--max-width", type=int, default=0, help="最大宽度像素（默认 640，0 表示不限制）")
    parser.add_argument("--start", type=float, default=0.0, help="起始时间（秒，默认 0）")
    parser.add_argument("--end", type=float, default=None, help="结束时间（秒，默认到结尾）")
    parser.add_argument("--loop", type=int, default=0, help="循环次数（0 为无限循环，默认 0）")

    args = parser.parse_args()
    max_w = None if args.max_width == 0 else args.max_width

    video_to_gif(
        video_path=args.input,
        output_path=args.output,
        angle=args.angle,
        step=args.step,
        fps=args.fps,
        max_width=max_w,
        start_sec=args.start,
        end_sec=args.end,
        loop=args.loop,
    )


if __name__ == "__main__":
    main()