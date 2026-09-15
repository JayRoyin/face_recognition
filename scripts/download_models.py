#!/usr/bin/env python3
"""
模型下载和转换脚本
用于下载 YOLOv8-Face 和 ArcFace 模型

用法:
    python3 download_models.py                    # 下载所有模型
    python3 download_models.py --yolo-only       # 仅下载 YOLO 模型
    python3 download_models.py --arcface-only     # 仅下载 ArcFace 模型
    python3 download_models.py -o /path/to/dir   # 指定输出目录
"""

import os
import sys
import argparse
import urllib.request

# Keep the caller's proxy configuration; do not force a machine-specific one.

def download_yolo_model(output_dir="models"):
    """下载 YOLOv8n ONNX 模型"""
    os.makedirs(output_dir, exist_ok=True)

    output_path = os.path.join(output_dir, "yolov8n.onnx")

    if os.path.exists(output_path) and os.path.getsize(output_path) > 1000000:
        print(f"YOLO model already exists at {output_path}")
        return output_path

    urls = [
        "https://github.com/ultralytics/assets/releases/download/v8.4.0/yolov8n.onnx",
    ]

    for url in urls:
        print(f"Downloading YOLOv8n model from {url}...")
        try:
            urllib.request.urlretrieve(url, output_path)
            if os.path.getsize(output_path) > 1000000:
                print(f"Model saved to {output_path} ({os.path.getsize(output_path)} bytes)")
                return output_path
            else:
                print(f"Downloaded file too small, retrying...")
                os.remove(output_path)
        except Exception as e:
            print(f"Failed to download from {url}: {e}")
            continue

    return None

def download_arcface_model(output_dir="models"):
    """下载 ArcFace buffalo_l ONNX 模型"""
    os.makedirs(output_dir, exist_ok=True)

    w600k_path = os.path.join(output_dir, "w600k_r50.onnx")

    if os.path.exists(w600k_path) and os.path.getsize(w600k_path) > 10000000:
        print(f"ArcFace model already exists at {w600k_path}")
        return w600k_path

    urls = [
        "https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_l.zip",
    ]

    for url in urls:
        print(f"Downloading ArcFace buffalo_l model from {url}...")
        try:
            zip_path = os.path.join(output_dir, "buffalo_l.zip")
            urllib.request.urlretrieve(url, zip_path)

            import zipfile
            with zipfile.ZipFile(zip_path, 'r') as zip_ref:
                zip_ref.extractall(output_dir)
            os.remove(zip_path)

            if os.path.exists(w600k_path):
                print(f"Model extracted to {w600k_path} ({os.path.getsize(w600k_path)} bytes)")
                return w600k_path
        except Exception as e:
            print(f"Failed to download from {url}: {e}")
            continue

    return None

def main():
    parser = argparse.ArgumentParser(description="Download face recognition models")
    parser.add_argument("--output-dir", "-o", default="models", help="Output directory for models")
    parser.add_argument("--yolo-only", action="store_true", help="Download YOLO model only")
    parser.add_argument("--arcface-only", action="store_true", help="Download ArcFace model only")

    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    print("="*50)
    print("Face Recognition Models Downloader")
    print("="*50)

    if args.yolo_only:
        path = download_yolo_model(args.output_dir)
        if path:
            print(f"\nYOLO model: {path}")
    elif args.arcface_only:
        path = download_arcface_model(args.output_dir)
        if path:
            print(f"\nArcFace model: {path}")
    else:
        print("\n[1/2] Downloading YOLOv8n model...")
        yolo_path = download_yolo_model(args.output_dir)

        print("\n[2/2] Downloading ArcFace buffalo_l model...")
        arcface_path = download_arcface_model(args.output_dir)

        print("\n" + "="*50)
        print("Download Summary")
        print("="*50)
        print(f"YOLOv8n:   {yolo_path or 'FAILED'}")
        print(f"ArcFace:    {arcface_path or 'FAILED'}")

if __name__ == "__main__":
    main()
