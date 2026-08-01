#include "ObjParser.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace AssetTool {

bool ObjParser::ParseFile(const std::string &filepath) {
    std::ifstream file(filepath);
    if (!file) {
        m_error = "Cannot open: " + filepath;
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return Parse(ss.str());
}

bool ObjParser::Parse(const std::string &content) {
    m_mesh = ObjMesh();
    m_error.clear();

    std::vector<float> rawPos, rawNorm, rawTex;
    // 暂存 face 的 v/t/n 三元组（1-based 索引）
    std::vector<std::vector<int>> facePos, faceTex, faceNorm;

    std::istringstream stream(content);
    std::string line;
    int lineNum = 0;

    while (std::getline(stream, line)) {
        lineNum++;
        // 去掉行首空白
        auto trimStart = line.find_first_not_of(" \t\r");
        if (trimStart == std::string::npos)
            continue;
        line = line.substr(trimStart);
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream ls(line);
        std::string cmd;
        ls >> cmd;

        if (cmd == "v") {
            float x, y, z;
            if (ls >> x >> y >> z) {
                rawPos.push_back(x);
                rawPos.push_back(y);
                rawPos.push_back(z);
            }
        } else if (cmd == "vn") {
            float x, y, z;
            if (ls >> x >> y >> z) {
                rawNorm.push_back(x);
                rawNorm.push_back(y);
                rawNorm.push_back(z);
            }
        } else if (cmd == "vt") {
            float u, v;
            if (ls >> u >> v) {
                rawTex.push_back(u);
                rawTex.push_back(v);
            }
        } else if (cmd == "f") {
            std::vector<int> posIdx, texIdx, normIdx;
            std::string token;
            while (ls >> token) {
                // 解析 v/t/n 或 v//vn 或 v 格式
                int v = 0, t = 0, n = 0;
                int parsed = sscanf(token.c_str(), "%d/%d/%d", &v, &t, &n);
                if (parsed == 1) {
                    // 只有 v
                } else if (parsed == 2) {
                    // v/t
                } else if (parsed == 3) {
                    // v/t/n 或 v//n
                }
                posIdx.push_back(v);
                texIdx.push_back(t);
                normIdx.push_back(n);
            }
            if (posIdx.size() >= 3) {
                facePos.push_back(posIdx);
                faceTex.push_back(texIdx);
                faceNorm.push_back(normIdx);
            }
        }
    }

    if (rawPos.empty()) {
        m_error = "No vertices found in OBJ";
        return false;
    }

    // 三角剖分（假设 OBJ 已是三角形，或简单扇剖分）
    for (size_t fi = 0; fi < facePos.size(); ++fi) {
        auto &pi = facePos[fi];
        auto &ti = faceTex[fi];
        auto &ni = faceNorm[fi];

        // 每个面至少有 3 个顶点，三角剖分：0-1-2, 0-2-3, ...
        for (size_t vi = 1; vi + 1 < pi.size(); ++vi) {
            // 3 个垂直顶点索引
            int idx[3] = {pi[0] - 1, pi[vi] - 1, pi[vi + 1] - 1};
            // 法线索引
            int nIdx[3] = {(int)ni.size() > 0 ? ni[0] - 1 : -1, (int)ni.size() > vi ? ni[vi] - 1 : -1,
                           (int)ni.size() > vi + 1 ? ni[vi + 1] - 1 : -1};
            // UV 索引
            int tIdx[3] = {(int)ti.size() > 0 ? ti[0] - 1 : -1, (int)ti.size() > vi ? ti[vi] - 1 : -1,
                           (int)ti.size() > vi + 1 ? ti[vi + 1] - 1 : -1};

            for (int j = 0; j < 3; ++j) {
                int viIdx = idx[j];
                if (viIdx < 0 || viIdx * 3 + 2 >= (int)rawPos.size())
                    continue;

                m_mesh.positions.push_back(rawPos[viIdx * 3]);
                m_mesh.positions.push_back(rawPos[viIdx * 3 + 1]);
                m_mesh.positions.push_back(rawPos[viIdx * 3 + 2]);

                if (nIdx[j] >= 0 && nIdx[j] * 3 + 2 < (int)rawNorm.size()) {
                    m_mesh.normals.push_back(rawNorm[nIdx[j] * 3]);
                    m_mesh.normals.push_back(rawNorm[nIdx[j] * 3 + 1]);
                    m_mesh.normals.push_back(rawNorm[nIdx[j] * 3 + 2]);
                }

                if (tIdx[j] >= 0 && tIdx[j] * 2 + 1 < (int)rawTex.size()) {
                    m_mesh.texcoords.push_back(rawTex[tIdx[j] * 2]);
                    m_mesh.texcoords.push_back(rawTex[tIdx[j] * 2 + 1]);
                }

                m_mesh.indices.push_back(static_cast<uint32_t>(m_mesh.indices.size()));
            }
        }
    }

    return !m_mesh.indices.empty();
}

} // namespace AssetTool
