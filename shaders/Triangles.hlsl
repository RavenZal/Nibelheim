cbuffer TransformConstants : register(b0)
{
    row_major float4x4 model;
    row_major float4x4 view;
    row_major float4x4 projection;
};

struct VertexShaderInput
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct VertexShaderOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
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

    output.color = input.color;

    return output;
}

float4 PSMain(VertexShaderOutput input) : SV_Target0
{
    return input.color;
}
