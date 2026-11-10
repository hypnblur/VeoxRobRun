#pragma once

// Shared colour helper for the config structs. It converts 8-bit channel
// values into the D2D1::ColorF form the overlay's ImGui conversion expects.
D2D1::ColorF Colour(UINT8 R, UINT8 G, UINT8 B, UINT8 A = 255);
