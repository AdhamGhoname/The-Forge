#pragma once
#include <stdio.h>
#include <vector>
#include <string>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "Mesh.h"
#include <unordered_map>

using namespace std;

class Model {
private:
    vector<Mesh> meshes;
    string directory;
    void loadModel(string path);
    void processNode(aiNode* currentNode, const aiScene* scene);
    Mesh processMesh(aiMesh* mesh, const aiScene* scene);
    vector<Texture> loadMaterialTextures(aiMaterial* material, aiTextureType type, string typeName);
    unordered_map<string, unsigned int> textures;
public:
    Model(const char* path);
    Model();
    static unsigned int TextureFromFile(string fullpath, string typeName);
    void render();
};
