cbuffer TransformConstants : register(b0)
{
    row_major float4x4 model;
    row_major float4x4 view;
    row_major float4x4 projection;
};

Texture2D<float4> baseColorTexture : register(t0);
SamplerState baseColorSampler : register(s0);

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
    const float4 sampledBaseColor = baseColorTexture.Sample(
        baseColorSampler,
        input.textureCoordinates
    );

    // Normal and tangent lighting comes later. For now, preserve consumption
    // of every expanded vertex attribute in the render-target alpha channel;
    // blending is disabled, so this does not alter the visible texture color.
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

    const float attributeCheck = saturate(
        0.5f * normalCheck + 0.5f * tangentCheck
    );

    return float4(sampledBaseColor.rgb, attributeCheck);
}
