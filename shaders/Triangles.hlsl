cbuffer TransformConstants : register(b0)
{
    row_major float4x4 model;
    row_major float4x4 view;
    row_major float4x4 projection;
};

struct VertexShaderInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 textureCoordinates : TEXCOORD;
    float4 tangent : TANGENT;
};

struct VertexShaderOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 textureCoordinates : TEXCOORD;
    float4 tangent : TANGENT;
};

VertexShaderOutput VSMain(VertexShaderInput input)
{
    VertexShaderOutput output;

    const float4 localPosition =
        float4(input.position, 1.0f);

    const float4 worldPosition =
        mul(localPosition, model);

    const float4 viewPosition =
        mul(worldPosition, view);

    output.position =
        mul(viewPosition, projection);

    output.normal = input.normal;
    output.textureCoordinates = input.textureCoordinates;
    output.tangent = input.tangent;

    return output;
}

float4 PSMain(VertexShaderOutput input) : SV_Target0
{
    // Until textures and lighting are introduced, this diagnostic color makes
    // the new vertex attributes observable. Red and green show the UV axes;
    // blue combines every normal and tangent component so DXC retains and the
    // GPU consumes all four input-layout elements.
    const float3 encodedNormal = input.normal * 0.5f + 0.5f;
    const float3 encodedTangent = input.tangent.xyz * 0.5f + 0.5f;

    const float normalCheck = dot(
        encodedNormal,
        float3(0.2f, 0.3f, 0.5f)
    );

    const float tangentCheck = 0.5f * dot(
        encodedTangent,
        float3(0.4f, 0.35f, 0.25f)
    ) + 0.5f * (input.tangent.w * 0.5f + 0.5f);

    return float4(
        saturate(input.textureCoordinates.x),
        saturate(input.textureCoordinates.y),
        saturate(0.5f * normalCheck + 0.5f * tangentCheck),
        1.0f
    );
}
