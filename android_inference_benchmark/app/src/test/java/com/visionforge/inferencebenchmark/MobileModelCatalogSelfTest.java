package com.visionforge.inferencebenchmark;
final class MobileModelCatalogSelfTest {
    static void run() {
        if (MobileModelCatalog.ALL.length!=1 || MobileModelCatalog.PRODUCTION_SELECTABLE.length!=1)
            throw new AssertionError("One body model only");
        MobileModelCatalog.Profile m=MobileModelCatalog.DEFAULT;
        if (!m.token.equals("ow_m_640") || !m.qnnLibrary.equals("libow_m_640_w8a16.so")
            || !m.quantizationToken.equals("W8A16")
            || !m.quantizationDescription.equals("weights=8 backbone_activations=16 head_activations=16 stride_constants=16 input=16 outputs=FP32")
            || m.inputWidth!=640 || m.inputHeight!=640 || m.anchorCount!=8400 || m.classCount!=1
            || m.bodyClassId!=0 || m.headClassId!=-1 || m.defaultAimTarget!=MobileAimTarget.HEAD
            || m.usesHeadBox(MobileAimTarget.HEAD) || !m.usesGeometricHead(MobileAimTarget.HEAD)
            || m.classIdFor(MobileAimTarget.HEAD)!=0 || m.targetYRatio(MobileAimTarget.HEAD)!=.18f
            || m.targetYRatio(MobileAimTarget.BODY)!=.5f)
            throw new AssertionError("Original body model and 18 percent head contract must remain intact");
        if (m.portableModelBytes!=80428634L
            || !m.portableModelSha256.equals("9C6E86CF5A04A33C8A53C4A506ECE371C67D350AF65568B00B8AF74D4E9630FE"))
            throw new AssertionError("Quantization must retain the original split ONNX weights");
        for(String stale:new String[]{"ow_m_320","ow_m_416","ow_n_640","ow_headbody_416_b320","removed-game"})
            if(MobileModelCatalog.forToken(stale)!=m) throw new AssertionError("Unknown model must use original body model");
    }
}
