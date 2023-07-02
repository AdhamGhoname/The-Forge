#pragma once
#include "../../../../Utilities/Math/MathTypes.h"
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>

struct Camera
{
public:
	float fov;
	float aspect_ratio;
	std::string projection;
	float nearZ;
	float farZ;
	Vector3 position;
	Vector3 forward;
	Vector3 up;
	Vector3 right;
	Vector3 eulerAngles;
	Vector2 lastMousePos;
	Vector3 eulerAngleLowerbound;
	Vector3 eulerAngleUpperbound;
	float mouseSensitivity;
	bool moved;


    Camera();
	Camera(Vector3 position,
		Vector3 forward,
		std::string projection,
		float nearZ,
		float farZ,
		float fov,
		float aspect_ratio,
		float mouseSensitivity,
		Vector3 eulerAngleLowerbound,
		Vector3 eulerAngleUpperbound);

	~Camera();

	void LookAt(Vector3 target);
	void SetEulerAngles(Vector3 eulerAngles);
	void SetPitch(float pitch);
	void SetYaw(float pitch);
	void SetAspectRatio(float ratio);
	void OffsetEulerAngles(Vector3 offset);
	void OffsetPitch(float offset);
	void OffsetYaw(float offset);
	void OnMouseMove(double xpos, double ypos);
	void OnMouseScroll(double xoffset, double yoffset);
	void Translate(Vector3 offset);
	void TranslateRelative(Vector3 offset);
	Vector3 GetPosition();
    Vector3 GetForward();
	Matrix4 GetViewMatrix();
	Matrix4 GetProjectionMatrix();
};
