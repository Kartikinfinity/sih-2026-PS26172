"""Train the "Sentinel" keyword spotter -- a small depthwise-separable CNN.

Architecture follows the DS-CNN family the plan names (ARM ML-KWS / MobileNet
style): a strided convolution to reduce the time-frequency grid, then stacked
depthwise-separable blocks, then global average pooling. Depthwise-separable
convolutions factor a standard convolution into a per-channel spatial filter
plus a 1x1 mix, cutting parameters and multiply-accumulates by roughly an order
of magnitude at similar accuracy -- which is what makes the model small enough
to quantise to int8 and run inside a 10 ms budget on an ESP32-S3.

Normalisation constants are computed on TRAIN ONLY and saved. They must be
applied identically on-device, or the host/device feature parity proven in
EXP-006 is destroyed one layer later.

Usage:
    python tools/train_kws.py [dataset.npz] [outdir]
"""

import json
import os
import sys

import numpy as np
import tensorflow as tf

SEED = 20260906
EPOCHS = 60
BATCH = 64
CLASSES = ["keyword", "unknown", "silence"]


def build_model(n_frames, n_mel, n_classes=3, ch=64):
    def ds_block(x, ch):
        x = tf.keras.layers.DepthwiseConv2D((3, 3), padding="same", use_bias=False)(x)
        x = tf.keras.layers.BatchNormalization()(x)
        x = tf.keras.layers.ReLU()(x)
        x = tf.keras.layers.Conv2D(ch, (1, 1), padding="same", use_bias=False)(x)
        x = tf.keras.layers.BatchNormalization()(x)
        x = tf.keras.layers.ReLU()(x)
        return x

    inp = tf.keras.layers.Input((n_frames, n_mel, 1))
    x = tf.keras.layers.Conv2D(ch, (10, 4), strides=(2, 2), padding="same",
                               use_bias=False)(inp)
    x = tf.keras.layers.BatchNormalization()(x)
    x = tf.keras.layers.ReLU()(x)
    for _ in range(4):
        x = ds_block(x, ch)
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    x = tf.keras.layers.Dropout(0.3)(x)
    out = tf.keras.layers.Dense(n_classes)(x)
    return tf.keras.Model(inp, out)


def report(name, y_true, logits, snr_tags=None):
    prob = tf.nn.softmax(logits).numpy()
    pred = prob.argmax(1)
    acc = float((pred == y_true).mean())
    print("")
    print("=== %s ===" % name)
    print("overall accuracy: %.4f (%d/%d)" % (acc, int((pred == y_true).sum()), len(y_true)))
    print("")
    print("confusion matrix (rows = truth, cols = predicted)")
    print("            " + "".join("%10s" % c for c in CLASSES))
    cm = np.zeros((3, 3), int)
    for t, p in zip(y_true, pred):
        cm[t, p] += 1
    for i, c in enumerate(CLASSES):
        print("%-12s" % c + "".join("%10d" % v for v in cm[i]))
    print("")
    print("%-10s %9s %9s %9s" % ("class", "precision", "recall", "f1"))
    for i, c in enumerate(CLASSES):
        tp = cm[i, i]
        prec = tp / max(cm[:, i].sum(), 1)
        rec = tp / max(cm[i, :].sum(), 1)
        f1 = 2 * prec * rec / max(prec + rec, 1e-9)
        print("%-10s %9.4f %9.4f %9.4f" % (c, prec, rec, f1))

    # What actually matters for a wake word: how often we miss the keyword, and
    # how often we fire when it was not said.
    kw = prob[:, 0]
    is_kw = (y_true == 0)
    print("")
    print("%-10s %12s %12s" % ("threshold", "miss rate", "false-fire rate"))
    for th in (0.5, 0.7, 0.9, 0.95, 0.99):
        miss = float((kw[is_kw] < th).mean())
        false = float((kw[~is_kw] >= th).mean())
        print("%-10.2f %11.4f %12.4f" % (th, miss, false))
    return acc, cm


def main():
    ds = sys.argv[1] if len(sys.argv) > 1 else "dataset/kws_dataset.npz"
    outdir = sys.argv[2] if len(sys.argv) > 2 else "model"
    os.makedirs(outdir, exist_ok=True)
    tf.keras.utils.set_random_seed(SEED)

    d = np.load(ds, allow_pickle=True)
    Xtr, ytr = d["Xtr"], d["ytr"]
    Xva, yva = d["Xva"], d["yva"]
    Xte, yte = d["Xte"], d["yte"]
    n_frames, n_mel = int(d["n_frames"]), int(d["n_mel"])

    # Normalisation from TRAIN ONLY. Using val/test statistics would leak.
    mean = float(Xtr.mean())
    std = float(Xtr.std())
    print("normalisation: mean %.6f  std %.6f  (train only)" % (mean, std))

    def prep(X):
        return ((X - mean) / std).astype(np.float32)[..., None]

    Xtr_n, Xva_n, Xte_n = prep(Xtr), prep(Xva), prep(Xte)

    model = build_model(n_frames, n_mel)
    model.compile(
        optimizer=tf.keras.optimizers.Adam(1e-3),
        loss=tf.keras.losses.SparseCategoricalCrossentropy(from_logits=True),
        metrics=["accuracy"])
    print("parameters: %d" % model.count_params())

    counts = np.bincount(ytr, minlength=3).astype(float)
    cw = {i: float(counts.sum() / (3 * max(counts[i], 1))) for i in range(3)}
    print("class weights:", {CLASSES[i]: round(v, 3) for i, v in cw.items()})

    cbs = [
        tf.keras.callbacks.EarlyStopping(monitor="val_loss", patience=12,
                                         restore_best_weights=True),
        tf.keras.callbacks.ReduceLROnPlateau(monitor="val_loss", factor=0.5,
                                             patience=5, min_lr=1e-5),
    ]
    hist = model.fit(Xtr_n, ytr, validation_data=(Xva_n, yva), epochs=EPOCHS,
                     batch_size=BATCH, class_weight=cw, callbacks=cbs, verbose=2)

    print("")
    print("best val accuracy: %.4f at epoch %d"
          % (max(hist.history["val_accuracy"]),
             int(np.argmax(hist.history["val_accuracy"])) + 1))

    report("VALIDATION", yva, model.predict(Xva_n, verbose=0))
    acc, cm = report("TEST (held out, never seen)", yte, model.predict(Xte_n, verbose=0))

    model.save(os.path.join(outdir, "kws_float.keras"))
    with open(os.path.join(outdir, "norm.json"), "w") as f:
        json.dump({"mean": mean, "std": std, "n_frames": n_frames,
                   "n_mel": n_mel, "classes": CLASSES}, f, indent=2)
    print("")
    print("saved %s/kws_float.keras and norm.json" % outdir)
    print("test accuracy %.4f" % acc)


if __name__ == "__main__":
    main()
