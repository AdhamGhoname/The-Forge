#pragma once
#include <stdio.h>
#include <string>
#include <vector>
#include "../../../../Utilities/Math/MathTypes.h"
#include "../../../../Graphics/Interfaces/IGraphics.h"



using namespace std;

struct Vertex {
    Vector3 Position;
	Vector3 Normal;
	Vector3 Tangent;
    Vector2 uv;
};

class Mesh {
private:
    void init();
public:
    vector<Vertex> vertices;
    vector<unsigned int> indices;
    vector<Texture*> textures;
    void render();
    Mesh(vector<Vertex> vertices, vector<unsigned int> indices, vector<Texture*> textures);
};
