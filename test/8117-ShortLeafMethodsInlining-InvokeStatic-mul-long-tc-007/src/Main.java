class Main {
    final static int iterations = 10;

    public static long simple_method(long jj, long ii) {
        ii--;
        try {
            throw new Exception("Test");
        } catch (Exception e) {
            ii++;
        }
        jj = ii * jj;
        return jj;
    }

    public static void main(String[] args) {
        long workJ = 2;
        long workK = 3;

        System.out.println("Initial workJ value is " + workJ);

        for(long i = 0; i < iterations; i++) {
            workJ = simple_method(workJ, workK) + i;
        }

        System.out.println("Final workJ value is " + workJ);
    }
}
