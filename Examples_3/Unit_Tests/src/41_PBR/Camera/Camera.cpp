#include "Camera.h"

Camera::Camera() {
    position = Vector3(0.0f);
    forward = Vector3(0.0f, 0.0f, 1.0f);
    projection = "persp";
    nearZ = 0.1f;
    farZ = 1000.0f;
    fov = 60.0f;
    aspect_ratio = 16.0f / 9.0f;
    mouseSensitivity = 0.1f;
    eulerAngleLowerbound = -Vector3(PI / 2.0f, PI / 2.0f, 0.0f);
    eulerAngleUpperbound = Vector3(PI / 2.0f, PI / 2.0f, 0.0f);
}

Camera::Camera(Vector3 position,
            Vector3 forward,
            std::string projection,
            float nearZ,
            float farZ,
            float fov,
            float aspect_ratio,
            float mouseSensitivity,
            Vector3 eulerAngleLowerbound,
            Vector3 eulerAngleUpperbound)
{
	this->position = position;
	this->forward = normalize(forward);
	this->projection = projection;
	this->nearZ = nearZ;
	this->farZ = farZ;
	this->fov = fov;
	this->aspect_ratio = aspect_ratio;
	this->mouseSensitivity = mouseSensitivity;
	this->eulerAngleLowerbound = eulerAngleLowerbound;
	this->eulerAngleUpperbound = eulerAngleUpperbound;
	float pitch = asin(this->forward.getY());
	float yaw = atan(this->forward.getZ() / this->forward.getX());
	this->eulerAngles = Vector3(pitch, yaw, 0.0f);
	this->moved = false;
}

Camera::~Camera() {
	this->projection.clear();
}

mat4 Camera::GetViewMatrix()
{
	Vector3 right = normalize(cross(this->forward, Vector3(0.0f, 1.0f, 0.0f)));
	Vector3 up = cross(right, this->forward);
	mat4 lookAt = Matrix4::lookAt(Point3(this->position), Point3(this->position - this->forward), up);
	return lookAt;
}

mat4 Camera::GetProjectionMatrix()
{
	if (this->projection == "persp") {
		return Matrix4::perspective(this->fov * PI / 180.0f, 1.0f / this->aspect_ratio, this->nearZ, this->farZ);
	}
	else
	{
		return Matrix4::orthographic(-1.0f, 1.0f, -1.0f / this->aspect_ratio, 1.0f / this->aspect_ratio, this->nearZ, this->farZ);
	}
}

void Camera::LookAt(Vector3 target)
{
	this->forward = normalize(target - this->position);
}

void Camera::SetAspectRatio(float ratio) {
	this->aspect_ratio = ratio;
}
void Camera::SetEulerAngles(Vector3 eulerAngles)
{
	//eulerAngles = min(this->eulerAngleUpperbound, eulerAngles);
	//eulerAngles = max(this->eulerAngleLowerbound, eulerAngles);
	this->eulerAngles = eulerAngles;
	this->forward.setX(cos(eulerAngles.getY()) * cos(eulerAngles.getX()));
	this->forward.setY(sin(eulerAngles.getX()));
	this->forward.setZ(sin(eulerAngles.getY()) * cos(eulerAngles.getX()));
}

void Camera::SetPitch(float pitch)
{
	this->SetEulerAngles(Vector3(pitch, this->eulerAngles.getY(), this->eulerAngles.getZ()));
}

void Camera::SetYaw(float yaw)
{
	this->SetEulerAngles(Vector3(this->eulerAngles.getX(), yaw, this->eulerAngles.getZ()));
}

void Camera::OffsetEulerAngles(Vector3 offset)
{
	this->SetEulerAngles(this->eulerAngles + offset);
}

void Camera::OffsetPitch(float offset)
{
	this->SetPitch(this->eulerAngles.getX() + offset);
}

void Camera::OffsetYaw(float offset)
{
	this->SetYaw(this->eulerAngles.getY() + offset);
}

void Camera::Translate(Vector3 offset)
{
	this->position += offset;
}

void Camera::TranslateRelative(Vector3 offset)
{
	this->position += (inverse(this->GetViewMatrix()) * vec4(offset, 0.0f)).getXYZ();
}

void Camera::OnMouseMove(double xpos, double ypos)
{   
	vec2 offset = vec2(xpos, ypos);

	offset *= this->mouseSensitivity / PI;
	this->OffsetEulerAngles(Vector3(offset.getY(), offset.getX(), 0));
}

void Camera::OnMouseScroll(double xoffset, double yoffset)
{
	float new_fov = this-> fov - yoffset * this->mouseSensitivity;
	this->fov = max(min(120.0f, new_fov), 1.0f);
}

Vector3 Camera::GetPosition() {
	return this->position;
}

Vector3 Camera::GetForward() {
    return this->forward;
}
