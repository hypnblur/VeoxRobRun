#pragma once

class Matrix4x4 {
public:
    float matrix[4][4];

    Matrix4x4 operator*(const Matrix4x4& other) const {
        Matrix4x4 result{};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                for (int k = 0; k < 4; k++)
                    result.matrix[i][j] += matrix[i][k] * other.matrix[k][j];
        return result;
    }
};
