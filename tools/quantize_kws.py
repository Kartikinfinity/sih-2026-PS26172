"""Quantise the trained model to int8 and measure what quantisation costs.

Full-integer quantisation: weights AND activations become int8, and the input
and output tensors are int8 too. This is what esp-tflite-micro and ESP-NN's
optimised kernels expect. A float or hybrid model would run, but would forfeit
the SIMD kernels and roughly the speed advantage that makes this feasible.

Quantisation is lossy. The point of this script is to MEASURE the loss on the
same held-out test set, not to assume it is negligible. It also emits the input
scale and zero-point, which the firmware must apply to its log-Mel features --
getting those wrong silently destroys accuracy while everything still runs.

Usage:
    python tools/quantize_kws.py [dataset.npz] [modeldir]
"""

import json
import os
import sys

import numpy as np
import tensorflow as tf

CLASSES = ["keyword", "unknown", "silence"]


def evaluate_tflite(path, X, y):
    interp = tf.lite.Interpreter(model_path=path)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]
    in_scale, in_zp = inp["quantization"]
    out_scale, out_zp = out["quantization"]

    probs = np.zeros((len(X), 3), np.float32)
    for i in range(len(X)):
        x = X[i:i + 1]
        if inp["dtype"] == np.int8:
            q = np.clip(np.round(x / in_scale + in_zp), -128, 127).astype(np.int8)
        else:
            q = x.astype(inp["dtype"])
        interp.set_tensor(inp["index"], q)
        interp.invoke()
        o = interp.get_tensor(out["index"])[0].astype(np.float32)
        if out["dtype"] == np.int8:
            o = (o - out_zp) * out_scale
        e = np.exp(o - o.max())
        probs[i] = e / e.sum()
    return probs, (in_scale, in_zp, out_scale, out_zp)


def report(name, y, probs):
    pred = probs.argmax(1)
    acc = float((pred == y).mean())
    cm = np.zeros((3, 3), int)
    for t, p in zip(y, pred):
        cm[t, p] += 1
    print("")
    print("=== %s ===" % name)
    print("accuracy               : %.4f" % acc)
    speech = (y != 2)
    print("accuracy excl. silence : %.4f" % float((pred[speech] == y[speech]).mean()))
    print("")
    print("            " + "".join("%10s" % c for c in CLASSES))
    for i, c in enumerate(CLASSES):
        print("%-12s" % c + "".join("%10d" % v for v in cm[i]))
    kw = probs[:, 0]
    is_kw = (y == 0)
    print("")
    print("%-10s %11s %14s" % ("threshold", "miss rate", "false-fire"))
    for th in (0.5, 0.7, 0.9, 0.95):
        print("%-10.2f %11.4f %14.4f"
              % (th, float((kw[is_kw] < th).mean()), float((kw[~is_kw] >= th).mean())))
    return acc, cm


def to_c_array(tflite_path, out_path, name="g_kws_model"):
    data = open(tflite_path, "rb").read()
    with open(out_path, "w") as f:
        f.write("// Auto-generated from %s -- do not edit by hand.\n"
                % os.path.basename(tflite_path))
        f.write("// %d bytes\n\n" % len(data))
        f.write('#include "kws_model.h"\n\n')
        f.write("alignas(16) const unsigned char %s[] = {\n" % name)
        for i in range(0, len(data), 12):
            f.write("  " + " ".join("0x%02x," % b for b in data[i:i + 12]) + "\n")
        f.write("};\n")
        f.write("const unsigned int %s_len = %d;\n" % (name, len(data)))
    return len(data)


def main():
    ds = sys.argv[1] if len(sys.argv) > 1 else "dataset/kws_dataset.npz"
    mdir = sys.argv[2] if len(sys.argv) > 2 else "model"

    d = np.load(ds, allow_pickle=True)
    Xtr, Xte, yte = d["Xtr"], d["Xte"], d["yte"]
    norm = json.load(open(os.path.join(mdir, "norm.json")))
    mean = np.asarray(norm["mean"], dtype=np.float64)
    std = np.asarray(norm["std"], dtype=np.float64)

    def prep(X):
        return ((X - mean) / std).astype(np.float32)[..., None]

    Xtr_n, Xte_n = prep(Xtr), prep(Xte)
    model = tf.keras.models.load_model(os.path.join(mdir, "kws_float.keras"))

    # Float reference on the same test set, for an honest before/after.
    fl = tf.nn.softmax(model.predict(Xte_n, verbose=0)).numpy()
    acc_f, _ = report("FLOAT32 (reference)", yte, fl)

    def representative():
        idx = np.random.default_rng(0).choice(len(Xtr_n), size=300, replace=False)
        for i in idx:
            yield [Xtr_n[i:i + 1]]

    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = representative
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tfl = conv.convert()

    path = os.path.join(mdir, "kws_int8.tflite")
    open(path, "wb").write(tfl)
    print("")
    print("int8 tflite: %d bytes (%.1f KB)" % (len(tfl), len(tfl) / 1024))

    q, (in_s, in_z, out_s, out_z) = evaluate_tflite(path, Xte_n, yte)
    acc_q, _ = report("INT8 (deployable)", yte, q)

    print("")
    print("=== QUANTISATION COST ===")
    print("float32 accuracy : %.4f" % acc_f)
    print("int8    accuracy : %.4f" % acc_q)
    print("delta            : %+.4f" % (acc_q - acc_f))
    agree = float((fl.argmax(1) == q.argmax(1)).mean())
    print("prediction agreement float vs int8 : %.4f" % agree)

    csrc = os.path.join(mdir, "kws_model.cc")
    n = to_c_array(path, csrc)
    with open(os.path.join(mdir, "kws_model.h"), "w") as f:
        f.write("#pragma once\nextern const unsigned char g_kws_model[];\n"
                "extern const unsigned int g_kws_model_len;\n")

    meta = dict(norm)
    meta["mean"] = [float(v) for v in np.atleast_1d(mean)]
    meta["std"] = [float(v) for v in np.atleast_1d(std)]
    meta.update({"input_scale": float(in_s), "input_zero_point": int(in_z),
                 "output_scale": float(out_s), "output_zero_point": int(out_z),
                 "tflite_bytes": int(len(tfl)),
                 "float_accuracy": acc_f, "int8_accuracy": acc_q})
    json.dump(meta, open(os.path.join(mdir, "model_meta.json"), "w"), indent=2)

    print("")
    print("input  quantisation: scale %.8f  zero_point %d" % (in_s, in_z))
    print("output quantisation: scale %.8f  zero_point %d" % (out_s, out_z))
    print("wrote %s (%d bytes) and model_meta.json" % (csrc, n))


if __name__ == "__main__":
    main()
