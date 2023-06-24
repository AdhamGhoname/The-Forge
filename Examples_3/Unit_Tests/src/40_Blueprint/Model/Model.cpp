#include "Model.h"
#include <filesystem>
#include <stb_image.h>

Model::Model(const char* path) {
    loadModel(path);
}

Model::Model() {}

void Model::render(Shader& shader) {
    for (auto& mesh : meshes) {
        mesh.render(shader);
    }
}

void Model::loadModel(string path) {
    Assimp::Importer importer;
    unsigned int postprocess_options = aiProcess_Triangulate |
        aiProcess_FlipUVs |
        aiProcess_JoinIdenticalVertices |
        aiProcess_CalcTangentSpace;
    const aiScene* scene = importer.ReadFile(path, postprocess_options);
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        cerr << "ERROR::ASSIMP::" << importer.GetErrorString() << endl;
        return;
    }
    fs::path p = path;
    directory = p.parent_path().generic_string();
    processNode(scene->mRootNode, scene);
}

void Model::processNode(aiNode *currentNode, const aiScene *scene) {
    for (int i = 0; i < currentNode->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[currentNode->mMeshes[i]];
        meshes.push_back(processMesh(mesh, scene));
    }
    for (int i = 0; i < currentNode->mNumChildren; i++) {
        processNode(currentNode->mChildren[i], scene);
    }
}

Mesh Model::processMesh(aiMesh *mesh, const aiScene *scene) {
    vector<Vertex> vertices;
    vector<unsigned int> indices;
    vector<Texture> textures;
    
    for (int i = 0; i < mesh->mNumVertices; i++) {
        Vertex vertex;
        vertex.Position = glm::vec3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);
        vertex.Normal = glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z);
        vertex.Tangent = glm::vec3(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z);
        if (mesh->mTextureCoords[0]) {
            vertex.uv = glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
        }
        else {
            vertex.uv = glm::vec2(0.0f);
        }
        vertices.push_back(vertex);
    }
    
    for (int i = 0; i < mesh->mNumFaces; i++) {
        aiFace face = mesh->mFaces[i];
        for (int j = 0; j < mesh->mFaces[i].mNumIndices; j++) {
            indices.push_back(mesh->mFaces[i].mIndices[j]);
        }
    }
    
    if (mesh->mMaterialIndex >= 0) {
        aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
        vector<Texture> diffuse = loadMaterialTextures(material, aiTextureType_DIFFUSE, "diffuse");
        vector<Texture> specular = loadMaterialTextures(material, aiTextureType_SPECULAR, "specular");
        vector<Texture> normal = loadMaterialTextures(material, aiTextureType_NORMALS, "normal");
        textures.insert(textures.end(), diffuse.begin(), diffuse.end());
        textures.insert(textures.end(), specular.begin(), specular.end());
        textures.insert(textures.end(), normal.begin(), normal.end());
    }
    
    return Mesh(vertices, indices, textures);
}

vector<Texture> Model::loadMaterialTextures(aiMaterial *material, aiTextureType type, string typeName) {
    vector<Texture> textures;
    for (int i = 0; i < material->GetTextureCount(type); i++) {
        aiString filename;
        material->GetTexture(type, i, &filename);
        Texture texture;
        string fullpath = (fs::path(directory) / fs::path(filename.C_Str())).generic_string();
        if (this->textures.count(fullpath)) {
            texture.id = this->textures.at(fullpath);

        }
        else {
            texture.id = TextureFromFile(fullpath, typeName);
            this->textures[fullpath] = texture.id;
        }
        texture.type = typeName;
        textures.push_back(texture);
    }
    return textures;
}

unsigned int Model::TextureFromFile(string fullpath, string typeName) {   
    unsigned int textureID;
    glGenTextures(1, &textureID);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    //stbi_set_flip_vertically_on_load(true);
    int width, height, channels;
    unsigned char* data = stbi_load(fullpath.c_str(), &width, &height, &channels, 0);
    if (data)
    {
        int format = channels == 3 ? 
            (typeName == "diffuse1" ? GL_RGBA : GL_RGB) : 
            (typeName == "diffuse1" ? GL_RGBA : GL_RGBA);
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    else
    {
        cout << "Couldn't load texture image: " << fullpath << endl;
    }
    stbi_image_free(data);
    glBindTexture(GL_TEXTURE_2D, 0);
    return textureID;
}
