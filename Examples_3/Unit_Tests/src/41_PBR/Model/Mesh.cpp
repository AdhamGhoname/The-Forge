#include "Mesh.h"
#include <sstream>

Mesh::Mesh(vector<Vertex> vertices, vector<unsigned int> indices, vector<Texture*> textures) {
    this->vertices = vertices;
    this->indices = indices;
    this->textures = textures;
    this->init();
}

void Mesh::init() {
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    
    glBindVertexArray(VAO);
    
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), &vertices[0], GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), &indices[0], GL_STATIC_DRAW);
    
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));
    
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Tangent));
    
    glBindVertexArray(0);
}

void Mesh::render(Shader& shader) {
    glBindVertexArray(VAO);
    //shader.use();
    int currentDiffuse = 0, currentSpecular = 0;
    
    for (int i = 0; i < this->textures.size(); i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, this->textures[i].id);
        string type = this->textures[i].type;
        if (type == "diffuse") {
            stringstream ss;
            ss << "material.diffuse[" << currentDiffuse++ << "]";
            shader.setUniform(ss.str().c_str(), i);
        }
        else if (type == "specular") {
            stringstream ss;
            ss << "material.specular[" << currentSpecular++ << "]";
            shader.setUniform(ss.str().c_str(), i);
        }
        else if (type == "normal") {
            shader.setUniform("material.normal", i);
        }
        else {
            // unknown type
            cerr << "Unknown type: " << type << endl;
        }
    }
    glDrawElements(GL_TRIANGLES, (int)indices.size(), GL_UNSIGNED_INT, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}
