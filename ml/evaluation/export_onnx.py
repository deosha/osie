# SPDX-License-Identifier: Apache-2.0
"""Rebuild a PyTorch module from an .osiem file and export it to ONNX
(batch size 1, inputs: hashed feature ids + per-token offsets), so ONNX Runtime
can be benchmarked against OSIE's custom C++ inference on identical weights.

  ml/.venv/bin/python ml/evaluation/export_onnx.py models/x.osiem models/x.onnx
"""
import sys

import numpy as np
import torch
import torch.nn as nn


def read_osiem(path):
    data = open(path, "rb").read()
    pos, header = 0, {}
    tensors = []
    while True:
        e = data.index(b"\n", pos)
        line = data[pos:e].decode()
        pos = e + 1
        if line == "data":
            break
        parts = line.split()
        if parts[0] == "tensor":
            tensors.append((parts[1], parts[2], [int(x) for x in parts[3:]]))
        else:
            header[parts[0]] = parts[1:]
    out = {}
    data_start = pos
    for name, dtype, shape in tensors:
        n = int(np.prod(shape))
        if dtype == "f32":
            out[name] = np.frombuffer(data, "<f4", n, pos).reshape(shape).copy()
            pos += 4 * n
        else:
            scale = np.frombuffer(data, "<f4", shape[0], pos)
            pos += 4 * shape[0]
            q = np.frombuffer(data, np.int8, n, pos).reshape(shape)
            pos += n
            pos += (-(pos - data_start)) % 4
            out[name] = (q.astype(np.float32) * scale[:, None]).astype(np.float32)
    return header, out


class Single(nn.Module):
    def __init__(self, header, w):
        super().__init__()
        self.arch = header["arch"][0]
        B, D = w["emb"].shape
        self.emb = nn.EmbeddingBag(B, D, mode="mean")
        self.emb.weight.data = torch.tensor(w["emb"])
        if self.arch == "bigru":
            H = w["gru_f_whh"].shape[1]
            self.rnn = nn.GRU(D, H, batch_first=True, bidirectional=True)
            for d, suf in (("f", ""), ("b", "_reverse")):
                getattr(self.rnn, f"weight_ih_l0{suf}").data = torch.tensor(w[f"gru_{d}_wih"])
                getattr(self.rnn, f"weight_hh_l0{suf}").data = torch.tensor(w[f"gru_{d}_whh"])
                getattr(self.rnn, f"bias_ih_l0{suf}").data = torch.tensor(w[f"gru_{d}_bih"])
                getattr(self.rnn, f"bias_hh_l0{suf}").data = torch.tensor(w[f"gru_{d}_bhh"])
        elif self.arch == "cnn":
            H = w["conv1_b"].shape[0]
            self.c1 = nn.Conv1d(D, H, 3, padding=1)
            self.c2 = nn.Conv1d(H, H, 3, padding=1)
            self.c1.weight.data, self.c1.bias.data = torch.tensor(w["conv1_w"]), torch.tensor(w["conv1_b"])
            self.c2.weight.data, self.c2.bias.data = torch.tensor(w["conv2_w"]), torch.tensor(w["conv2_b"])
        else:
            H = w["proj_b"].shape[0]
            self.proj = nn.Linear(D, H)
            self.proj.weight.data, self.proj.bias.data = torch.tensor(w["proj_w"]), torch.tensor(w["proj_b"])
        self.intent = nn.Linear(*w["intent_w"].shape[::-1])
        self.intent.weight.data, self.intent.bias.data = torch.tensor(w["intent_w"]), torch.tensor(w["intent_b"])
        self.tag = nn.Linear(*w["tag_w"].shape[::-1])
        self.tag.weight.data, self.tag.bias.data = torch.tensor(w["tag_w"]), torch.tensor(w["tag_b"])

    def forward(self, ids, offsets):
        x = self.emb(ids, offsets).unsqueeze(0)  # [1, T, D]
        if self.arch == "bigru":
            h, _ = self.rnn(x)
        elif self.arch == "cnn":
            h = torch.relu(self.c2(torch.relu(self.c1(x.transpose(1, 2))))).transpose(1, 2)
        else:
            h = torch.relu(self.proj(x))
        pooled = torch.cat([h.mean(1), h.max(1).values], dim=1)
        return torch.softmax(self.intent(pooled), 1), torch.softmax(self.tag(h[0]), 1)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    header, w = read_osiem(src)
    m = Single(header, w).eval()
    ids = torch.tensor([1, 2, 3, 4, 5, 6], dtype=torch.long)
    offs = torch.tensor([0, 3], dtype=torch.long)
    torch.onnx.export(m, (ids, offs), dst, input_names=["ids", "offsets"], output_names=["intent", "tags"],
                      dynamic_axes={"ids": {0: "n"}, "offsets": {0: "t"}, "tags": {0: "t"}}, opset_version=17,
                      dynamo=False)
    print(f"exported {header['arch'][0]} -> {dst}")


if __name__ == "__main__":
    main()
