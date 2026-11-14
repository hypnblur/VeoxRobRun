#pragma once
#include <cmath>
class Vector2
{
public:
	Vector2();
	Vector2(float _x, float _y);
	~Vector2();

	float x, y;

	Vector2 operator *(const Vector2& a) const;
	Vector2 operator /(const Vector2& a) const;
	Vector2 operator +(const Vector2& a) const;
	Vector2 operator -(const Vector2& a) const;
	bool operator ==(const Vector2& a) const;
	bool operator !=(const Vector2& a) const;

	bool IsZero() const;

	static Vector2 Zero();

};

class Vector3
{
public:
	Vector3();
	Vector3(float _x, float _y, float _z);
	~Vector3();

	float x, y, z;

	Vector3 operator *(const Vector3& a) const;
	Vector3 operator *(float f);
	Vector3 operator /(const Vector3& a) const;
	Vector3 operator /(float f);
	Vector3 operator +(const Vector3& a) const;
	Vector3 operator -(const Vector3& a) const;
	bool operator ==(const Vector3& a) const;
	bool operator !=(const Vector3& a) const;

	bool IsZero() const;

	static float Dot(Vector3 left, Vector3 right);
	static int FormattedDistance(Vector3 a, Vector3 b);
	static Vector3 Zero();
	static Vector3 Lerp(Vector3 a, Vector3 b, float t);

	float Length() const;
	float LengthSqr() const;

	Vector3 Clamp() const;
};

// Generic Euclidean distance between two vectors, shared by the reader, the
// ESP and the aim code so every consumer measures identically.
inline float Distance(const Vector2& a, const Vector2& b)
{
    const auto difference = Vector2(a.x - b.x, a.y - b.y);
    return std::sqrt(std::pow(difference.x, 2.0f) + std::pow(difference.y, 2.0f));
}

inline float Distance(const Vector3& a, const Vector3& b)
{
    const auto difference = Vector3(a.x - b.x, a.y - b.y, a.z - b.z);
    return std::sqrt(std::pow(difference.x, 2.0f) + std::pow(difference.y, 2.0f) + std::pow(difference.z, 2.0f));
}

struct ViewMatrix
{
public:
	float matrix[4][4];

	Vector3 Transform(const Vector3 vector) const;
};
