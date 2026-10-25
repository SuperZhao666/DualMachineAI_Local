package com.visionforge.inferencebenchmark;

/** Immutable Java-side mirror of model lifecycle data required before JNI is ready. */
public final class MobileModelCatalog {
    public static final Profile OVERWATCH_2 = new Profile(
            "ow_m_640", "libow_m_640_w8a16.so",
            "W8A16",
            "weights=8 backbone_activations=16 head_activations=16 stride_constants=16 input=16 outputs=FP32",
            "ow_m_640.onnx",
            80428634L, "9C6E86CF5A04A33C8A53C4A506ECE371C67D350AF65568B00B8AF74D4E9630FE",
            640, 640, 8400, 1,
            0.20f, 0.45f, 0.10f, 0.85f, 0.60f, 0.5f, 127,
            MobileAimTarget.HEAD,
            bodyTarget(MobileAimTarget.BODY, 0, -1),
            geometricHeadTarget(MobileAimTarget.HEAD, 0));
    public static final Profile DEFAULT = OVERWATCH_2;
    public static final Profile[] ALL = { OVERWATCH_2 };
    public static final Profile[] PRODUCTION_SELECTABLE = ALL;

    private MobileModelCatalog() {}

    private static AimTargetContract bodyTarget(
            MobileAimTarget target, int bodyClassId, int headClassId) {
        return new AimTargetContract(
                target, bodyClassId, headClassId, bodyClassId, false, false, 0.50f);
    }

    private static AimTargetContract headBoxTarget(
            MobileAimTarget target, int bodyClassId, int headClassId) {
        return new AimTargetContract(
                target, bodyClassId, headClassId, headClassId, true, false, 0.50f);
    }

    private static AimTargetContract geometricHeadTarget(
            MobileAimTarget target, int bodyClassId) {
        return new AimTargetContract(
                target, bodyClassId, -1, bodyClassId, false, true, 0.18f);
    }

    private static AimTargetContract directClassTarget(
            MobileAimTarget target, int bodyClassId, int headClassId, int selectedClassId) {
        return new AimTargetContract(
                target, bodyClassId, headClassId, selectedClassId, false, false, 0.50f);
    }

    public static Profile forToken(String token) {
        if (token != null) {
            for (Profile profile : ALL) {
                if (profile.token.equals(token)) return profile;
            }
        }
        return DEFAULT;
    }

    public static Profile forProductionToken(String token) {
        if (token != null) {
            for (Profile profile : PRODUCTION_SELECTABLE) {
                if (profile.token.equals(token)) return profile;
            }
        }
        return DEFAULT;
    }

    public static boolean isProductionSelectable(Profile profile) {
        if (profile == null) return false;
        for (Profile candidate : PRODUCTION_SELECTABLE) {
            if (candidate == profile) return true;
        }
        return false;
    }

    public static final class Profile {
        public final String token;
        public final String qnnLibrary;
        public final String quantizationToken;
        public final String quantizationDescription;
        public final String portableModelAsset;
        public final long portableModelBytes;
        public final String portableModelSha256;
        public final int inputWidth;
        public final int inputHeight;
        public final int anchorCount;
        public final int classCount;
        public final int bodyClassId;
        public final int headClassId;
        public final float defaultConfidence;
        public final float nmsIou;
        public final float minimumConfidence;
        public final float maximumConfidence;
        public final float defaultControlGain;
        public final float defaultDeadzonePixels;
        public final int defaultMaximumAxisDelta;
        public final MobileAimTarget defaultAimTarget;
        public final MobileAimTarget[] aimTargets;
        private final AimTargetContract[] aimTargetContracts;

        Profile(
                String token,
                String qnnLibrary,
                String quantizationToken,
                String quantizationDescription,
                String portableModelAsset,
                long portableModelBytes,
                String portableModelSha256,
                int inputWidth,
                int inputHeight,
                int anchorCount,
                int classCount,
                float defaultConfidence,
                float nmsIou,
                float minimumConfidence,
                float maximumConfidence,
                float defaultControlGain,
                float defaultDeadzonePixels,
                int defaultMaximumAxisDelta,
                MobileAimTarget defaultAimTarget,
                AimTargetContract... aimTargetContracts) {
            if (token == null || token.isBlank() || qnnLibrary == null || qnnLibrary.isBlank()
                    || quantizationToken == null || !quantizationToken.matches("[A-Z0-9_]+")
                    || quantizationDescription == null || quantizationDescription.isBlank()
                    || portableModelAsset == null || portableModelAsset.isBlank()
                    || portableModelBytes <= 0L
                    || portableModelSha256 == null
                    || !portableModelSha256.matches("[0-9A-F]{64}")
                    || inputWidth <= 0 || inputHeight <= 0
                    || anchorCount <= 0 || classCount <= 0
                    || !validThreshold(defaultConfidence)
                    || !validThreshold(nmsIou)
                    || !validThreshold(minimumConfidence)
                    || !validThreshold(maximumConfidence)
                    || minimumConfidence > defaultConfidence
                    || defaultConfidence > maximumConfidence
                    || !Float.isFinite(defaultControlGain) || defaultControlGain < 0.01f
                    || defaultControlGain > 2.0f
                    || !Float.isFinite(defaultDeadzonePixels) || defaultDeadzonePixels < 0.0f
                    || defaultDeadzonePixels > 64.0f
                    || defaultMaximumAxisDelta < 1 || defaultMaximumAxisDelta > 127
                    || defaultAimTarget == null || aimTargetContracts == null
                    || aimTargetContracts.length == 0) {
                throw new IllegalArgumentException("invalid mobile model profile");
            }
            this.token = token;
            this.qnnLibrary = qnnLibrary;
            this.quantizationToken = quantizationToken;
            this.quantizationDescription = quantizationDescription;
            this.portableModelAsset = portableModelAsset;
            this.portableModelBytes = portableModelBytes;
            this.portableModelSha256 = portableModelSha256;
            this.inputWidth = inputWidth;
            this.inputHeight = inputHeight;
            this.anchorCount = anchorCount;
            this.classCount = classCount;
            this.defaultConfidence = defaultConfidence;
            this.nmsIou = nmsIou;
            this.minimumConfidence = minimumConfidence;
            this.maximumConfidence = maximumConfidence;
            this.defaultControlGain = defaultControlGain;
            this.defaultDeadzonePixels = defaultDeadzonePixels;
            this.defaultMaximumAxisDelta = defaultMaximumAxisDelta;
            this.defaultAimTarget = defaultAimTarget;
            this.aimTargetContracts = aimTargetContracts.clone();
            this.aimTargets = new MobileAimTarget[aimTargetContracts.length];
            for (int index = 0; index < aimTargetContracts.length; index++) {
                AimTargetContract contract = aimTargetContracts[index];
                if (!contract.isValidFor(classCount) || containsTarget(contract.target, index)) {
                    throw new IllegalArgumentException("invalid mobile aim target contract");
                }
                this.aimTargets[index] = contract.target;
            }
            AimTargetContract defaultContract = contractFor(defaultAimTarget);
            if (defaultContract == null) {
                throw new IllegalArgumentException("default aim target is unsupported");
            }
            this.bodyClassId = defaultContract.bodyClassId;
            this.headClassId = defaultContract.headClassId;
        }

        public boolean supports(MobileAimTarget target) {
            if (target == null) return false;
            for (MobileAimTarget candidate : aimTargets) {
                if (candidate == target) return true;
            }
            return false;
        }

        public int classIdFor(MobileAimTarget target) {
            return requireContract(target).selectedClassId;
        }

        public int bodyClassIdFor(MobileAimTarget target) {
            return requireContract(target).bodyClassId;
        }

        public int headClassIdFor(MobileAimTarget target) {
            return requireContract(target).headClassId;
        }

        public boolean usesHeadBox(MobileAimTarget target) {
            return requireContract(target).usesHeadBox;
        }

        public boolean usesGeometricHead(MobileAimTarget target) {
            return requireContract(target).usesGeometricHead;
        }

        public float targetYRatio(MobileAimTarget target) {
            return requireContract(target).targetYRatio;
        }

        private AimTargetContract requireContract(MobileAimTarget target) {
            AimTargetContract contract = contractFor(target);
            if (contract == null) throw new IllegalArgumentException("unsupported aim target");
            return contract;
        }

        private AimTargetContract contractFor(MobileAimTarget target) {
            if (target == null) return null;
            for (AimTargetContract contract : aimTargetContracts) {
                if (contract.target == target) return contract;
            }
            return null;
        }

        private boolean containsTarget(MobileAimTarget target, int beforeIndex) {
            for (int index = 0; index < beforeIndex; index++) {
                if (aimTargetContracts[index].target == target) return true;
            }
            return false;
        }

        private static boolean validThreshold(float value) {
            return Float.isFinite(value) && value >= 0.0f && value <= 1.0f;
        }
    }

    private static final class AimTargetContract {
        final MobileAimTarget target;
        final int bodyClassId;
        final int headClassId;
        final int selectedClassId;
        final boolean usesHeadBox;
        final boolean usesGeometricHead;
        final float targetYRatio;

        AimTargetContract(
                MobileAimTarget target,
                int bodyClassId,
                int headClassId,
                int selectedClassId,
                boolean usesHeadBox,
                boolean usesGeometricHead,
                float targetYRatio) {
            this.target = target;
            this.bodyClassId = bodyClassId;
            this.headClassId = headClassId;
            this.selectedClassId = selectedClassId;
            this.usesHeadBox = usesHeadBox;
            this.usesGeometricHead = usesGeometricHead;
            this.targetYRatio = targetYRatio;
        }

        boolean isValidFor(int classCount) {
            boolean classesValid = target != null
                    && bodyClassId >= 0 && bodyClassId < classCount
                    && headClassId >= -1 && headClassId < classCount
                    && selectedClassId >= 0 && selectedClassId < classCount;
            boolean geometryValid = !(usesHeadBox && usesGeometricHead)
                    && (!usesHeadBox || headClassId >= 0 && selectedClassId == headClassId)
                    && (!usesGeometricHead || headClassId < 0
                    && selectedClassId == bodyClassId);
            return classesValid && geometryValid && Float.isFinite(targetYRatio)
                    && targetYRatio >= 0.0f && targetYRatio <= 1.0f;
        }
    }
}
