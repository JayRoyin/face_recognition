#!/usr/bin/env python3
"""
Convert RetinaFace ONNX model from Sigmoid outputs to Identity outputs.

The InsightFace det_10g.onnx ends with Sigmoid operations on the face scores.
This is fine for ONNX Runtime but causes issues with OpenCV 4.5.4 dnn due
to a bug in getLayerShapesRecursively. Replacing Sigmoid with Identity
preserves the numerical output while making the model compatible with
older OpenCV versions.

Usage:
    python3 scripts/fix_retinaface_onnx.py \\
        --input models/det_10g.onnx \\
        --output models/det_10g_opencv.onnx
"""
import argparse
import sys

try:
    import onnx
except ImportError:
    print("ERROR: onnx package is required. Install with: pip install onnx")
    sys.exit(1)


def fix_model(input_path: str, output_path: str) -> None:
    print(f"Loading {input_path}...")
    model = onnx.load(input_path)
    graph = model.graph

    n_converted = 0
    for node in graph.node:
        if node.op_type == "Sigmoid":
            node.op_type = "Identity"
            # Remove attributes not needed by Identity
            for attr in list(node.attribute):
                if attr.name not in ("axis",):
                    node.attribute.remove(attr)
            n_converted += 1
            print(f"  Converted {node.name} to Identity")

    if n_converted == 0:
        print("WARNING: No Sigmoid nodes found - model not modified")
    else:
        print(f"Converted {n_converted} Sigmoid -> Identity")

    print(f"Saving {output_path}...")
    onnx.save(model, output_path)
    print("Done")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Fix RetinaFace ONNX for OpenCV 4.5.4")
    parser.add_argument("--input", "-i", required=True, help="Input ONNX path")
    parser.add_argument("--output", "-o", required=True, help="Output ONNX path")
    args = parser.parse_args()
    fix_model(args.input, args.output)
