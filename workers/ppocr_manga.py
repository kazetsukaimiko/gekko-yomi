#!/usr/bin/env python3
"""Persistent PP-OCRv6 manga worker.

Same protocol as meiki_ocr.py: one JSON line, then stride*height raw BGRA
bytes, then one JSON line back. Weights download from Kellenok/PP-OCRv6_manga
(Apache-2.0) on first use and stay out of this repository.

The published ONNX example feeds OpenCV BGR with ImageNet mean and std.
This worker does the same, on onnxruntime. No Paddle runtime and no ROCm
provider. The detection model returns a line box. Recognition is CTC over
that crop, so there are no character boxes.
"""

import json
import os
import sys

os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "1")
os.environ.setdefault("HF_HUB_DISABLE_TELEMETRY", "1")

REPO = "Kellenok/PP-OCRv6_manga"
DET_NAME = "det/manga_det_v0.2.onnx"
REC_NAME = "rec/manga_rec_v0.2.onnx"
DICT_NAME = "ppocrv6_dict.txt"


def read_line(stream):
    data = bytearray()
    while True:
        chunk = stream.read(1)
        if not chunk:
            return None
        if chunk == b"\n":
            return data.decode("utf-8")
        data.extend(chunk)


def write_payload(payload):
    sys.stdout.buffer.write((json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"))
    sys.stdout.buffer.flush()


def providers(ort):
    # CUDA when that build has it, otherwise CPU. Never ROCm.
    available = set(ort.get_available_providers())
    chosen = [name for name in ("CUDAExecutionProvider", "CPUExecutionProvider") if name in available]
    return chosen or ["CPUExecutionProvider"]


def load_vocab(path):
    with open(path, encoding="utf-8") as handle:
        lines = [line.strip("\r\n") for line in handle]
    # Index 0 is the CTC blank. The model was trained with a trailing space.
    return ["blank"] + lines + [" "]


def token_probs(logits):
    import numpy as np

    # This recognizer already returns a probability per class. A second softmax
    # would crush a 0.99 into noise across an 18000-wide vocabulary.
    if np.all(logits >= -1e-4) and np.allclose(logits.sum(axis=-1), 1.0, atol=1e-2):
        return logits
    shifted = logits - logits.max(axis=-1, keepdims=True)
    exp = np.exp(shifted)
    return exp / np.maximum(exp.sum(axis=-1, keepdims=True), 1e-12)


def ctc_decode(logits, vocab):
    import numpy as np

    probs = token_probs(logits)
    indices = np.argmax(logits, axis=-1)
    chars = []
    confs = []
    previous = -1
    for index, choice in enumerate(indices.tolist()):
        if choice != 0 and choice != previous and 0 <= choice < len(vocab):
            chars.append(vocab[choice])
            confs.append(float(probs[index, choice]))
        previous = choice
    confidence = float(sum(confs) / len(confs)) if confs else 0.0
    return "".join(chars), confidence


class Engine:
    def __init__(self):
        import cv2
        import numpy as np
        import onnxruntime as ort
        import pyclipper
        from huggingface_hub import snapshot_download

        self.cv2 = cv2
        self.np = np
        self.pyclipper = pyclipper
        root = snapshot_download(REPO, allow_patterns=["config.json", DICT_NAME, DET_NAME, REC_NAME])
        options = ort.SessionOptions()
        options.log_severity_level = 3
        chosen = providers(ort)
        self.det = ort.InferenceSession(os.path.join(root, DET_NAME), options, providers=chosen)
        self.rec = ort.InferenceSession(os.path.join(root, REC_NAME), options, providers=chosen)
        self.det_input = self.det.get_inputs()[0].name
        self.rec_input = self.rec.get_inputs()[0].name
        self.vocab = load_vocab(os.path.join(root, DICT_NAME))

    def detect_boxes(self, bgr):
        cv2 = self.cv2
        np = self.np
        height, width = bgr.shape[:2]
        if height < 2 or width < 2:
            return []
        long_side = max(height, width)
        short_side = min(height, width)
        if long_side / short_side > 2:
            ratio = min(1.0, (0.75 * 960.0 * 960.0 / (height * width)) ** 0.5)
        else:
            ratio = 960.0 / long_side
        resized_h = max(int(round(height * ratio / 32) * 32), 32)
        resized_w = max(int(round(width * ratio / 32) * 32), 32)
        image = cv2.resize(bgr, (resized_w, resized_h)).astype(np.float32) / 255.0
        mean = np.array([0.485, 0.456, 0.406], dtype=np.float32)
        std = np.array([0.229, 0.224, 0.225], dtype=np.float32)
        image = (image - mean) / std
        tensor = image.transpose(2, 0, 1)[np.newaxis, ...]
        pred = self.det.run(None, {self.det_input: tensor})[0]
        while pred.ndim > 2:
            pred = pred[0]
        thresh = 0.15
        box_thresh = 0.25
        unclip_ratio = 1.4
        mask = (pred > thresh).astype(np.uint8) * 255
        contours, _ = cv2.findContours(mask, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
        boxes = []
        for contour in contours:
            points = contour.squeeze(1)
            if points.ndim != 2 or points.shape[0] < 4:
                continue
            if self.box_score(pred, points) < box_thresh:
                continue
            points = self.unclip(points, unclip_ratio)
            if points is None or len(points) == 0:
                continue
            rect = cv2.minAreaRect(points.astype(np.float32))
            _, (box_w, box_h), _ = rect
            if min(box_w, box_h) < 3:
                continue
            quad = cv2.boxPoints(rect).astype(np.float32)
            map_h, map_w = pred.shape[:2]
            quad[:, 0] = np.clip(np.round(quad[:, 0] / map_w * width), 0, width)
            quad[:, 1] = np.clip(np.round(quad[:, 1] / map_h * height), 0, height)
            boxes.append(quad)
        boxes.sort(key=lambda quad: (float(quad[:, 1].min()), float(quad[:, 0].min())))
        return boxes

    def box_score(self, pred, box):
        cv2 = self.cv2
        np = self.np
        height, width = pred.shape[:2]
        points = box.copy().astype(np.int32)
        xmin = int(np.clip(np.floor(points[:, 0].min()), 0, width - 1))
        xmax = int(np.clip(np.ceil(points[:, 0].max()), 0, width - 1))
        ymin = int(np.clip(np.floor(points[:, 1].min()), 0, height - 1))
        ymax = int(np.clip(np.ceil(points[:, 1].max()), 0, height - 1))
        score_mask = np.zeros((ymax - ymin + 1, xmax - xmin + 1), dtype=np.uint8)
        points[:, 0] -= xmin
        points[:, 1] -= ymin
        cv2.fillPoly(score_mask, [points.reshape(-1, 2)], 1)
        return cv2.mean(pred[ymin : ymax + 1, xmin : xmax + 1], score_mask)[0]

    def unclip(self, box, ratio):
        np = self.np
        pyclipper = self.pyclipper
        xs, ys = box[:, 0], box[:, 1]
        area = 0.5 * abs(np.dot(xs, np.roll(ys, -1)) - np.dot(ys, np.roll(xs, -1)))
        perimeter = np.linalg.norm(np.roll(box, -1, axis=0) - box, axis=1).sum()
        if area <= 0 or perimeter <= 0:
            return None
        offset = pyclipper.PyclipperOffset()
        offset.AddPath(box.tolist(), pyclipper.JT_ROUND, pyclipper.ET_CLOSEDPOLYGON)
        paths = offset.Execute(area * ratio / perimeter)
        if len(paths) != 1:
            return None
        return np.asarray(paths[0], dtype=np.float32)

    def recognize_crop(self, crop):
        cv2 = self.cv2
        np = self.np
        if crop.size == 0:
            return "", 0.0, False
        vertical = crop.shape[0] > crop.shape[1]
        if vertical:
            crop = cv2.rotate(crop, cv2.ROTATE_90_COUNTERCLOCKWISE)
        crop_h, crop_w = crop.shape[:2]
        natural_w = int(round(48.0 * crop_w / max(1, crop_h)))
        target_w = max(16, min(2400 if natural_w > 2000 else 640, natural_w))
        image = cv2.resize(crop, (target_w, 48)).astype(np.float32) / 255.0
        image = ((image - 0.5) / 0.5).transpose(2, 0, 1)[np.newaxis, ...]
        logits = self.rec.run(None, {self.rec_input: image})[0]
        while logits.ndim > 2:
            logits = logits[0]
        text, confidence = ctc_decode(logits, self.vocab)
        return text, confidence, vertical

    def run(self, bgr):
        runs = []
        height, width = bgr.shape[:2]
        for quad in self.detect_boxes(bgr):
            xs, ys = quad[:, 0], quad[:, 1]
            x1 = max(0, int(xs.min()))
            y1 = max(0, int(ys.min()))
            x2 = min(width, int(xs.max()))
            y2 = min(height, int(ys.max()))
            if x2 <= x1 or y2 <= y1:
                continue
            text, confidence, vertical = self.recognize_crop(bgr[y1:y2, x1:x2])
            if not text:
                continue
            runs.append(
                {
                    "text": text,
                    "vertical": vertical,
                    "confidence": confidence,
                    "box": [x1, y1, x2 - x1, y2 - y1],
                }
            )
        return runs


def handle(engine, meta, pixels):
    import numpy as np

    width = int(meta["width"])
    height = int(meta["height"])
    stride = int(meta["stride"])
    image = np.frombuffer(pixels, dtype=np.uint8).reshape(height, stride)
    bgra = image[:, : width * 4].reshape(height, width, 4)
    bgr = bgra[:, :, :3]
    return {"runs": engine.run(bgr)}


def main():
    try:
        engine = Engine()
    except Exception as exc:
        write_payload({"error": "ppocr manga worker failed to import: " + str(exc)})
        return 1

    stream = sys.stdin.buffer
    while True:
        header = read_line(stream)
        if header is None:
            return 0
        try:
            meta = json.loads(header)
            nbytes = int(meta["stride"]) * int(meta["height"])
            pixels = stream.read(nbytes)
            if len(pixels) != nbytes:
                payload = {"error": "short pixel read"}
            else:
                payload = handle(engine, meta, pixels)
        except Exception as exc:
            payload = {"error": "ppocr manga worker failed: " + str(exc)}
        write_payload(payload)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(0)
