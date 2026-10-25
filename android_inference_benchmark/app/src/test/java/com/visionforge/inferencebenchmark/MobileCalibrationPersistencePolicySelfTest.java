package com.visionforge.inferencebenchmark;

final class MobileCalibrationPersistencePolicySelfTest {
    static void run() {
        double[] good={.709,.707,24_000,.99,.99,1,10,1,20_000,28_000,1};
        require(MobileCalibrationPersistencePolicy.evaluate(good).writable);
        double[] ambiguous=good.clone(); ambiguous[7]=0; ambiguous[2]=80_000;
        ambiguous[8]=8_000; ambiguous[9]=100_000; ambiguous[10]=24;
        require(!MobileCalibrationPersistencePolicy.evaluate(ambiguous).writable);
        require(MobileCalibrationPersistencePolicy.evaluate(ambiguous).reason.equals("delay_not_identifiable"));
        require(!MobileCalibrationPersistencePolicy.evaluate(java.util.Arrays.copyOf(good,7)).writable);
        require(!MobileCalibrationPersistencePolicy.evaluate(null).writable);
        for(int index=0;index<11;++index) {
            double[] invalid=good.clone();invalid[index]=Double.NaN;
            require(!MobileCalibrationPersistencePolicy.evaluate(invalid).writable);
            invalid[index]=Double.POSITIVE_INFINITY;
            require(!MobileCalibrationPersistencePolicy.evaluate(invalid).writable);
        }
        for(int index:new int[]{5,7,6,10}) {
            double[] invalid=good.clone();invalid[index]=.5;
            require(!MobileCalibrationPersistencePolicy.evaluate(invalid).writable);
        }
        for(double[] range:new double[][]{{30_000,20_000},{25_000,28_000},{8_000,23_999},{0,100_000},{8_000,100_001}}) {
            double[] invalid=good.clone();invalid[8]=range[0];invalid[9]=range[1];
            require(!MobileCalibrationPersistencePolicy.evaluate(invalid).writable);
        }
        for(double count:new double[]{0,25,-1}) {
            double[] invalid=good.clone();invalid[10]=count;
            require(!MobileCalibrationPersistencePolicy.evaluate(invalid).writable);
        }
        MobileCalibrationPersistencePolicy.Decision snapshot=MobileCalibrationPersistencePolicy.evaluate(good);
        good[2]=80_000;require(snapshot.values[2]==24_000);
        MobileCalibrationPersistencePolicy.SkipThrottle throttle=new MobileCalibrationPersistencePolicy.SkipThrottle();
        require(throttle.shouldReport("ow|usb",100));
        for(int i=1;i<100;++i)require(!throttle.shouldReport("ow|usb",100+i));
        require(!throttle.shouldReport("ow|usb",5_099));
        require(throttle.shouldReport("ow|usb",5_100));
        require(throttle.shouldReport("ow|bt",5_101));
        require(throttle.shouldReport("ow|bt",0));
    }
    private static void require(boolean condition) { if (!condition) throw new AssertionError("calibration persistence evidence violated"); }
}
