#include "Marlin.h"
#include "Configuration.h"
#include "language_all.h"
#include "mesh_bed_calibration.h"
#include "mesh_bed_leveling.h"
#include "stepper.h"
#include "ultralcd.h"
// #include "qr_solve.h"

extern float home_retract_mm_ext(int axis);

uint8_t world2machine_correction_mode;
float   world2machine_rotation_and_skew[2][2];
float   world2machine_rotation_and_skew_inv[2][2];
float   world2machine_shift[2];

// Weight of the Y coordinate for the least squares fitting of the bed induction sensor targets.
// Only used for the first row of the points, which may not befully in reach of the sensor.
#define WEIGHT_FIRST_ROW (0.2f)

#define BED_ZERO_REF_X (- 22.f + X_PROBE_OFFSET_FROM_EXTRUDER)
#define BED_ZERO_REF_Y (- 0.6f + Y_PROBE_OFFSET_FROM_EXTRUDER)

// Scaling of the real machine axes against the programmed dimensions in the firmware.
// The correction is tiny, here around 0.5mm on 250mm length.
#define MACHINE_AXIS_SCALE_X ((250.f + 0.5f) / 250.f)
#define MACHINE_AXIS_SCALE_Y ((250.f + 0.5f) / 250.f)

#define BED_SKEW_ANGLE_MILD         (0.12f * M_PI / 180.f)
#define BED_SKEW_ANGLE_EXTREME      (0.25f * M_PI / 180.f)

#define BED_CALIBRATION_POINT_OFFSET_MAX_EUCLIDIAN  (0.8f)
#define BED_CALIBRATION_POINT_OFFSET_MAX_1ST_ROW_X  (0.8f)
#define BED_CALIBRATION_POINT_OFFSET_MAX_1ST_ROW_Y  (1.5f)

// Positions of the bed reference points in the machine coordinates, referenced to the P.I.N.D.A sensor.
// The points are ordered in a zig-zag fashion to speed up the calibration.
const float bed_ref_points[] PROGMEM = {
    13.f  - BED_ZERO_REF_X,   6.4f - BED_ZERO_REF_Y,
    115.f - BED_ZERO_REF_X,   6.4f - BED_ZERO_REF_Y,
    216.f - BED_ZERO_REF_X,   6.4f - BED_ZERO_REF_Y,

    216.f - BED_ZERO_REF_X, 104.4f - BED_ZERO_REF_Y,
    115.f - BED_ZERO_REF_X, 104.4f - BED_ZERO_REF_Y,
    13.f  - BED_ZERO_REF_X, 104.4f - BED_ZERO_REF_Y,

    13.f  - BED_ZERO_REF_X, 202.4f - BED_ZERO_REF_Y,
    115.f - BED_ZERO_REF_X, 202.4f - BED_ZERO_REF_Y,
    216.f - BED_ZERO_REF_X, 202.4f - BED_ZERO_REF_Y
};

// Positions of the bed reference points in the machine coordinates, referenced to the P.I.N.D.A sensor.
// The points are the following: center front, center right, center rear, center left.
const float bed_ref_points_4[] PROGMEM = {
    115.f - BED_ZERO_REF_X,   6.4f - BED_ZERO_REF_Y,
    216.f - BED_ZERO_REF_X, 104.4f - BED_ZERO_REF_Y,
    115.f - BED_ZERO_REF_X, 202.4f - BED_ZERO_REF_Y,
    13.f  - BED_ZERO_REF_X, 104.4f - BED_ZERO_REF_Y
};

//#define Y_MIN_POS_FOR_BED_CALIBRATION (MANUAL_Y_HOME_POS-0.2f)
#define Y_MIN_POS_FOR_BED_CALIBRATION (Y_MIN_POS)

static inline float sqr(float x) { return x * x; }


#if 0
// Linear Least Squares fitting of the bed to the measured induction points.
// This method will not maintain a unity length of the machine axes.
// This may be all right if the sensor points are measured precisely,
// but it will stretch or shorten the machine axes if the measured data is not precise enough.
bool calculate_machine_skew_and_offset_LS(
    // Matrix of maximum 9 2D points (18 floats)
    const float  *measured_pts,
    uint8_t       npts,
    const float  *true_pts,
    // Resulting correction matrix.
    float        *vec_x,
    float        *vec_y,
    float        *cntr,
    // Temporary values, 49-18-(2*3)=25 floats
//    , float *temp
    int8_t        verbosity_level
    )
{
    if (verbosity_level >= 10) {
        // Show the initial state, before the fitting.
        SERIAL_ECHOPGM("X vector, initial: ");
        MYSERIAL.print(vec_x[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_x[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("Y vector, initial: ");
        MYSERIAL.print(vec_y[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_y[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("center, initial: ");
        MYSERIAL.print(cntr[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(cntr[1], 5);
        SERIAL_ECHOLNPGM("");

        for (uint8_t i = 0; i < npts; ++ i) {
            SERIAL_ECHOPGM("point #");
            MYSERIAL.print(int(i));
            SERIAL_ECHOPGM(" measured: (");
            MYSERIAL.print(measured_pts[i*2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(measured_pts[i*2+1], 5);
            SERIAL_ECHOPGM("); target: (");
            MYSERIAL.print(pgm_read_float(true_pts+i*2  ), 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(pgm_read_float(true_pts+i*2+1), 5);
            SERIAL_ECHOPGM("), error: ");
            MYSERIAL.print(sqrt(
                sqr(pgm_read_float(true_pts+i*2  ) - measured_pts[i*2  ]) +
                sqr(pgm_read_float(true_pts+i*2+1) - measured_pts[i*2+1])), 5);
            SERIAL_ECHOLNPGM("");
        }
        delay_keep_alive(100);
    }

    {
        // Create covariance matrix for A, collect the right hand side b.
        float A[3][3] = { 0.f };
        float b[3] = { 0.f };
        float acc;
        for (uint8_t r = 0; r < 3; ++ r) {
            for (uint8_t c = 0; c < 3; ++ c) {
                acc = 0;
                for (uint8_t i = 0; i < npts; ++ i) {
                    float a = (r == 2) ? 1.f : measured_pts[2 * i + r];
                    float b = (c == 2) ? 1.f : measured_pts[2 * i + c];
                    acc += a * b;
                }
                A[r][c] = acc;
            }
            acc = 0.f;
            for (uint8_t i = 0; i < npts; ++ i) {
                float a = (r == 2) ? 1.f : measured_pts[2 * i + r];
                float b = pgm_read_float(true_pts+i*2);
                acc += a * b;
            }
            b[r] = acc;
        }
        // Solve the linear equation for ax, bx, cx.
        float x[3] = { 0.f };
        for (uint8_t iter = 0; iter < 100; ++ iter) {
            x[0] = (b[0] - A[0][1] * x[1] - A[0][2] * x[2]) / A[0][0];
            x[1] = (b[1] - A[1][0] * x[0] - A[1][2] * x[2]) / A[1][1];
            x[2] = (b[2] - A[2][0] * x[0] - A[2][1] * x[1]) / A[2][2];
        }
        // Store the result to the output variables.
        vec_x[0] = x[0];
        vec_y[0] = x[1];
        cntr[0] = x[2];

        // Recalculate A and b for the y values.
        // Note the weighting of the first row of values.
        for (uint8_t r = 0; r < 3; ++ r) {
            for (uint8_t c = 0; c < 3; ++ c) {
                acc = 0;
                for (uint8_t i = 0; i < npts; ++ i) {
                    float w = (i < 3) ? WEIGHT_FIRST_ROW : 1.f;
                    float a = (r == 2) ? 1.f : measured_pts[2 * i + r];
                    float b = (c == 2) ? 1.f : measured_pts[2 * i + c];
                    acc += a * b * w;
                }
                A[r][c] = acc;
            }
            acc = 0.f;
            for (uint8_t i = 0; i < npts; ++ i) {
                float w = (i < 3) ? WEIGHT_FIRST_ROW : 1.f;
                float a = (r == 2) ? 1.f : measured_pts[2 * i + r];
                float b = pgm_read_float(true_pts+i*2+1);
                acc += w * a * b;
            }
            b[r] = acc;
        }
        // Solve the linear equation for ay, by, cy.
        x[0] = 0.f, x[1] = 0.f; x[2] = 0.f;
        for (uint8_t iter = 0; iter < 100; ++ iter) {
            x[0] = (b[0] - A[0][1] * x[1] - A[0][2] * x[2]) / A[0][0];
            x[1] = (b[1] - A[1][0] * x[0] - A[1][2] * x[2]) / A[1][1];
            x[2] = (b[2] - A[2][0] * x[0] - A[2][1] * x[1]) / A[2][2];
        }
        // Store the result to the output variables.
        vec_x[1] = x[0];
        vec_y[1] = x[1];
        cntr[1] = x[2];
    }

    if (verbosity_level >= 10) {
        // Show the adjusted state, before the fitting.
        SERIAL_ECHOPGM("X vector new, inverted: ");
        MYSERIAL.print(vec_x[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_x[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("Y vector new, inverted: ");
        MYSERIAL.print(vec_y[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_y[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("center new, inverted: ");
        MYSERIAL.print(cntr[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(cntr[1], 5);
        SERIAL_ECHOLNPGM("");
        delay_keep_alive(100);

        SERIAL_ECHOLNPGM("Error after correction: ");
        for (uint8_t i = 0; i < npts; ++ i) {
            float x = vec_x[0] * measured_pts[i*2] + vec_y[0] * measured_pts[i*2+1] + cntr[0];
            float y = vec_x[1] * measured_pts[i*2] + vec_y[1] * measured_pts[i*2+1] + cntr[1];
            SERIAL_ECHOPGM("point #");
            MYSERIAL.print(int(i));
            SERIAL_ECHOPGM(" measured: (");
            MYSERIAL.print(measured_pts[i*2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(measured_pts[i*2+1], 5);
            SERIAL_ECHOPGM("); corrected: (");
            MYSERIAL.print(x, 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(y, 5);
            SERIAL_ECHOPGM("); target: (");
            MYSERIAL.print(pgm_read_float(true_pts+i*2  ), 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(pgm_read_float(true_pts+i*2+1), 5);
            SERIAL_ECHOPGM("), error: ");
            MYSERIAL.print(sqrt(sqr(pgm_read_float(true_pts+i*2)-x)+sqr(pgm_read_float(true_pts+i*2+1)-y)));
            SERIAL_ECHOLNPGM("");
        }
    }

#if 0
    // Normalize the vectors. We expect, that the machine axes may be skewed a bit, but the distances are correct.
    // l shall be very close to 1 already.
    float l = sqrt(vec_x[0]*vec_x[0] + vec_x[1] * vec_x[1]);
    vec_x[0] /= l;
    vec_x[1] /= l;
    SERIAL_ECHOPGM("Length of the X vector: ");
    MYSERIAL.print(l, 5);
    SERIAL_ECHOLNPGM("");
    l = sqrt(vec_y[0]*vec_y[0] + vec_y[1] * vec_y[1]);
    vec_y[0] /= l;
    vec_y[1] /= l;
    SERIAL_ECHOPGM("Length of the Y vector: ");
    MYSERIAL.print(l, 5);
    SERIAL_ECHOLNPGM("");

    // Recalculate the center using the adjusted vec_x/vec_y
    {
        cntr[0] = 0.f;
        cntr[1] = 0.f;
        for (uint8_t i = 0; i < npts; ++ i) {
            cntr[0] += measured_pts[2 * i    ] - pgm_read_float(true_pts+i*2) * vec_x[0] - pgm_read_float(true_pts+i*2+1) * vec_y[0];
            cntr[1] += measured_pts[2 * i + 1] - pgm_read_float(true_pts+i*2) * vec_x[1] - pgm_read_float(true_pts+i*2+1) * vec_y[1];
        }
        cntr[0] /= float(npts);
        cntr[1] /= float(npts);
    }

    SERIAL_ECHOPGM("X vector new, inverted, normalized: ");
    MYSERIAL.print(vec_x[0], 5);
    SERIAL_ECHOPGM(", ");
    MYSERIAL.print(vec_x[1], 5);
    SERIAL_ECHOLNPGM("");

    SERIAL_ECHOPGM("Y vector new, inverted, normalized: ");
    MYSERIAL.print(vec_y[0], 5);
    SERIAL_ECHOPGM(", ");
    MYSERIAL.print(vec_y[1], 5);
    SERIAL_ECHOLNPGM("");

    SERIAL_ECHOPGM("center new, inverted, normalized: ");
    MYSERIAL.print(cntr[0], 5);
    SERIAL_ECHOPGM(", ");
    MYSERIAL.print(cntr[1], 5);
    SERIAL_ECHOLNPGM("");
#endif

    // Invert the transformation matrix made of vec_x, vec_y and cntr.
    {
        float d = vec_x[0] * vec_y[1] - vec_x[1] * vec_y[0];
        float Ainv[2][2] = { 
            {   vec_y[1] / d, - vec_y[0] / d },
            { - vec_x[1] / d,   vec_x[0] / d }
        };
        float cntrInv[2] = {
            - Ainv[0][0] * cntr[0] - Ainv[0][1] * cntr[1],
            - Ainv[1][0] * cntr[0] - Ainv[1][1] * cntr[1]
        };
        vec_x[0] = Ainv[0][0];
        vec_x[1] = Ainv[1][0];
        vec_y[0] = Ainv[0][1];
        vec_y[1] = Ainv[1][1];
        cntr[0] = cntrInv[0];
        cntr[1] = cntrInv[1];
    }

    if (verbosity_level >= 1) {
        // Show the adjusted state, before the fitting.
        SERIAL_ECHOPGM("X vector, adjusted: ");
        MYSERIAL.print(vec_x[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_x[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("Y vector, adjusted: ");
        MYSERIAL.print(vec_y[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_y[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("center, adjusted: ");
        MYSERIAL.print(cntr[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(cntr[1], 5);
        SERIAL_ECHOLNPGM("");
        delay_keep_alive(100);
    }

    if (verbosity_level >= 2) {
        SERIAL_ECHOLNPGM("Difference after correction: ");
        for (uint8_t i = 0; i < npts; ++ i) {
            float x = vec_x[0] * pgm_read_float(true_pts+i*2) + vec_y[0] * pgm_read_float(true_pts+i*2+1) + cntr[0];
            float y = vec_x[1] * pgm_read_float(true_pts+i*2) + vec_y[1] * pgm_read_float(true_pts+i*2+1) + cntr[1];
            SERIAL_ECHOPGM("point #");
            MYSERIAL.print(int(i));
            SERIAL_ECHOPGM("measured: (");
            MYSERIAL.print(measured_pts[i*2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(measured_pts[i*2+1], 5);
            SERIAL_ECHOPGM("); measured-corrected: (");
            MYSERIAL.print(x, 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(y, 5);
            SERIAL_ECHOPGM("); target: (");
            MYSERIAL.print(pgm_read_float(true_pts+i*2  ), 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(pgm_read_float(true_pts+i*2+1), 5);
            SERIAL_ECHOPGM("), error: ");
            MYSERIAL.print(sqrt(sqr(measured_pts[i*2]-x)+sqr(measured_pts[i*2+1]-y)));
            SERIAL_ECHOLNPGM("");
        }
        delay_keep_alive(100);
    }

    return true;
}

#else

// Non-Linear Least Squares fitting of the bed to the measured induction points
// using the Gauss-Newton method.
// This method will maintain a unity length of the machine axes,
// which is the correct approach if the sensor points are not measured precisely.
BedSkewOffsetDetectionResultType calculate_machine_skew_and_offset_LS(
    // Matrix of maximum 9 2D points (18 floats)
    const float  *measured_pts,
    uint8_t       npts,
    const float  *true_pts,
    // Resulting correction matrix.
    float        *vec_x,
    float        *vec_y,
    float        *cntr,
    // Temporary values, 49-18-(2*3)=25 floats
    //    , float *temp
    int8_t        verbosity_level
    )
{
    if (verbosity_level >= 10) {
        // Show the initial state, before the fitting.
        SERIAL_ECHOPGM("X vector, initial: ");
        MYSERIAL.print(vec_x[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_x[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("Y vector, initial: ");
        MYSERIAL.print(vec_y[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_y[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("center, initial: ");
        MYSERIAL.print(cntr[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(cntr[1], 5);
        SERIAL_ECHOLNPGM("");

        for (uint8_t i = 0; i < npts; ++i) {
            SERIAL_ECHOPGM("point #");
            MYSERIAL.print(int(i));
            SERIAL_ECHOPGM(" measured: (");
            MYSERIAL.print(measured_pts[i * 2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(measured_pts[i * 2 + 1], 5);
            SERIAL_ECHOPGM("); target: (");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2), 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2 + 1), 5);
            SERIAL_ECHOPGM("), error: ");
            MYSERIAL.print(sqrt(
                sqr(pgm_read_float(true_pts + i * 2) - measured_pts[i * 2]) +
                sqr(pgm_read_float(true_pts + i * 2 + 1) - measured_pts[i * 2 + 1])), 5);
            SERIAL_ECHOLNPGM("");
        }
        delay_keep_alive(100);
    }

    // Run some iterations of the Gauss-Newton method of non-linear least squares.
    // Initial set of parameters:
    // X,Y offset
    cntr[0] = 0.f;
    cntr[1] = 0.f;
    // Rotation of the machine X axis from the bed X axis.
    float a1 = 0;
    // Rotation of the machine Y axis from the bed Y axis.
    float a2 = 0;
    for (int8_t iter = 0; iter < 100; ++iter) {
        float c1 = cos(a1) * MACHINE_AXIS_SCALE_X;
        float s1 = sin(a1) * MACHINE_AXIS_SCALE_X;
        float c2 = cos(a2) * MACHINE_AXIS_SCALE_Y;
        float s2 = sin(a2) * MACHINE_AXIS_SCALE_Y;
        // Prepare the Normal equation for the Gauss-Newton method.
        float A[4][4] = { 0.f };
        float b[4] = { 0.f };
        float acc;
        for (uint8_t r = 0; r < 4; ++r) {
            for (uint8_t c = 0; c < 4; ++c) {
                acc = 0;
                // J^T times J
                for (uint8_t i = 0; i < npts; ++i) {
                    // First for the residuum in the x axis:
                    if (r != 1 && c != 1) {
                        float a = 
                             (r == 0) ? 1.f :
                            ((r == 2) ? (-s1 * measured_pts[2 * i]) :
                                        (-c2 * measured_pts[2 * i + 1]));
                        float b = 
                             (c == 0) ? 1.f :
                            ((c == 2) ? (-s1 * measured_pts[2 * i]) :
                                        (-c2 * measured_pts[2 * i + 1]));
                        acc += a * b;
                    }
                    // Second for the residuum in the y axis. 
                    // The first row of the points have a low weight, because their position may not be known
                    // with a sufficient accuracy.
                    if (r != 0 && c != 0) {
                        float a = 
                             (r == 1) ? 1.f :
                            ((r == 2) ? ( c1 * measured_pts[2 * i]) :
                                        (-s2 * measured_pts[2 * i + 1]));
                        float b = 
                             (c == 1) ? 1.f :
                            ((c == 2) ? ( c1 * measured_pts[2 * i]) :
                                        (-s2 * measured_pts[2 * i + 1]));
                        float w = (i < 3) ? WEIGHT_FIRST_ROW : 1.f;
                        acc += a * b * w;
                    }
                }
                A[r][c] = acc;
            }
            // J^T times f(x)
            acc = 0.f;
            for (uint8_t i = 0; i < npts; ++i) {
                {
                    float j = 
                         (r == 0) ? 1.f :
                        ((r == 1) ? 0.f :
                        ((r == 2) ? (-s1 * measured_pts[2 * i]) :
                                    (-c2 * measured_pts[2 * i + 1])));
                    float fx = c1 * measured_pts[2 * i] - s2 * measured_pts[2 * i + 1] + cntr[0] - pgm_read_float(true_pts + i * 2);
                    acc += j * fx;
                }
                {
                    float j = 
                         (r == 0) ? 0.f :
                        ((r == 1) ? 1.f :
                        ((r == 2) ? ( c1 * measured_pts[2 * i]) :
                                    (-s2 * measured_pts[2 * i + 1])));
                    float fy = s1 * measured_pts[2 * i] + c2 * measured_pts[2 * i + 1] + cntr[1] - pgm_read_float(true_pts + i * 2 + 1);
                    float w = (i < 3) ? WEIGHT_FIRST_ROW : 1.f;
                    acc += j * fy * w;
                }
            }
            b[r] = -acc;
        }

        // Solve for h by a Gauss iteration method.
        float h[4] = { 0.f };
        for (uint8_t gauss_iter = 0; gauss_iter < 100; ++gauss_iter) {
            h[0] = (b[0] - A[0][1] * h[1] - A[0][2] * h[2] - A[0][3] * h[3]) / A[0][0];
            h[1] = (b[1] - A[1][0] * h[0] - A[1][2] * h[2] - A[1][3] * h[3]) / A[1][1];
            h[2] = (b[2] - A[2][0] * h[0] - A[2][1] * h[1] - A[2][3] * h[3]) / A[2][2];
            h[3] = (b[3] - A[3][0] * h[0] - A[3][1] * h[1] - A[3][2] * h[2]) / A[3][3];
        }

        // and update the current position with h.
        // It may be better to use the Levenberg-Marquart method here,
        // but because we are very close to the solution alread,
        // the simple Gauss-Newton non-linear Least Squares method works well enough.
        cntr[0] += h[0];
        cntr[1] += h[1];
        a1 += h[2];
        a2 += h[3];

        if (verbosity_level >= 20) {
            SERIAL_ECHOPGM("iteration: ");
            MYSERIAL.print(iter, 0);
            SERIAL_ECHOPGM("correction vector: ");
            MYSERIAL.print(h[0], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(h[1], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(h[2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(h[3], 5);
            SERIAL_ECHOLNPGM("");
            SERIAL_ECHOPGM("corrected x/y: ");
            MYSERIAL.print(cntr[0], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(cntr[0], 5);
            SERIAL_ECHOLNPGM("");
            SERIAL_ECHOPGM("corrected angles: ");
            MYSERIAL.print(180.f * a1 / M_PI, 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(180.f * a2 / M_PI, 5);
            SERIAL_ECHOLNPGM("");
        }
    }

    vec_x[0] =  cos(a1) * MACHINE_AXIS_SCALE_X;
    vec_x[1] =  sin(a1) * MACHINE_AXIS_SCALE_X;
    vec_y[0] = -sin(a2) * MACHINE_AXIS_SCALE_Y;
    vec_y[1] =  cos(a2) * MACHINE_AXIS_SCALE_Y;

    BedSkewOffsetDetectionResultType result = BED_SKEW_OFFSET_DETECTION_PERFECT;
    {
        float angleDiff = fabs(a2 - a1);
        if (angleDiff > BED_SKEW_ANGLE_MILD)
            result = (angleDiff > BED_SKEW_ANGLE_EXTREME) ?
                BED_SKEW_OFFSET_DETECTION_SKEW_EXTREME :
                BED_SKEW_OFFSET_DETECTION_SKEW_MILD;
        if (fabs(a1) > BED_SKEW_ANGLE_EXTREME ||
            fabs(a2) > BED_SKEW_ANGLE_EXTREME)
            result = BED_SKEW_OFFSET_DETECTION_SKEW_EXTREME;
    }

    if (verbosity_level >= 1) {
        SERIAL_ECHOPGM("correction angles: ");
        MYSERIAL.print(180.f * a1 / M_PI, 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(180.f * a2 / M_PI, 5);
        SERIAL_ECHOLNPGM("");
    }

    if (verbosity_level >= 10) {
        // Show the adjusted state, before the fitting.
        SERIAL_ECHOPGM("X vector new, inverted: ");
        MYSERIAL.print(vec_x[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_x[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("Y vector new, inverted: ");
        MYSERIAL.print(vec_y[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_y[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("center new, inverted: ");
        MYSERIAL.print(cntr[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(cntr[1], 5);
        SERIAL_ECHOLNPGM("");
        delay_keep_alive(100);

        SERIAL_ECHOLNPGM("Error after correction: ");
    }

    // Measure the error after correction.
    for (uint8_t i = 0; i < npts; ++i) {
        float x = vec_x[0] * measured_pts[i * 2] + vec_y[0] * measured_pts[i * 2 + 1] + cntr[0];
        float y = vec_x[1] * measured_pts[i * 2] + vec_y[1] * measured_pts[i * 2 + 1] + cntr[1];
        float errX = sqr(pgm_read_float(true_pts + i * 2) - x);
        float errY = sqr(pgm_read_float(true_pts + i * 2 + 1) - y);
        float err = sqrt(errX + errY);
        if (i < 3) {
            if (sqrt(errX) > BED_CALIBRATION_POINT_OFFSET_MAX_1ST_ROW_X ||
                sqrt(errY) > BED_CALIBRATION_POINT_OFFSET_MAX_1ST_ROW_Y)
                result = BED_SKEW_OFFSET_DETECTION_FAILED;
        } else {
            if (err > BED_CALIBRATION_POINT_OFFSET_MAX_EUCLIDIAN)
                result = BED_SKEW_OFFSET_DETECTION_FAILED;
        }
        if (verbosity_level >= 10) {
            SERIAL_ECHOPGM("point #");
            MYSERIAL.print(int(i));
            SERIAL_ECHOPGM(" measured: (");
            MYSERIAL.print(measured_pts[i * 2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(measured_pts[i * 2 + 1], 5);
            SERIAL_ECHOPGM("); corrected: (");
            MYSERIAL.print(x, 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(y, 5);
            SERIAL_ECHOPGM("); target: (");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2), 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2 + 1), 5);
            SERIAL_ECHOPGM("), error: ");
            MYSERIAL.print(err);
            SERIAL_ECHOLNPGM("");
        }
    }

    if (result == BED_SKEW_OFFSET_DETECTION_PERFECT && fabs(a1) < BED_SKEW_ANGLE_MILD && fabs(a2) < BED_SKEW_ANGLE_MILD) {
        if (verbosity_level > 0)
            SERIAL_ECHOLNPGM("Very little skew detected. Disabling skew correction.");
        vec_x[0] = MACHINE_AXIS_SCALE_X;
        vec_x[1] = 0.f;
        vec_y[0] = 0.f;
        vec_y[1] = MACHINE_AXIS_SCALE_Y;
    }

    // Invert the transformation matrix made of vec_x, vec_y and cntr.
    {
        float d = vec_x[0] * vec_y[1] - vec_x[1] * vec_y[0];
        float Ainv[2][2] = {
            { vec_y[1] / d, -vec_y[0] / d },
            { -vec_x[1] / d, vec_x[0] / d }
        };
        float cntrInv[2] = {
            -Ainv[0][0] * cntr[0] - Ainv[0][1] * cntr[1],
            -Ainv[1][0] * cntr[0] - Ainv[1][1] * cntr[1]
        };
        vec_x[0] = Ainv[0][0];
        vec_x[1] = Ainv[1][0];
        vec_y[0] = Ainv[0][1];
        vec_y[1] = Ainv[1][1];
        cntr[0] = cntrInv[0];
        cntr[1] = cntrInv[1];
    }

    if (verbosity_level >= 1) {
        // Show the adjusted state, before the fitting.
        SERIAL_ECHOPGM("X vector, adjusted: ");
        MYSERIAL.print(vec_x[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_x[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("Y vector, adjusted: ");
        MYSERIAL.print(vec_y[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_y[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("center, adjusted: ");
        MYSERIAL.print(cntr[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(cntr[1], 5);
        SERIAL_ECHOLNPGM("");
        delay_keep_alive(100);
    }

    if (verbosity_level >= 2) {
        SERIAL_ECHOLNPGM("Difference after correction: ");
        for (uint8_t i = 0; i < npts; ++i) {
            float x = vec_x[0] * pgm_read_float(true_pts + i * 2) + vec_y[0] * pgm_read_float(true_pts + i * 2 + 1) + cntr[0];
            float y = vec_x[1] * pgm_read_float(true_pts + i * 2) + vec_y[1] * pgm_read_float(true_pts + i * 2 + 1) + cntr[1];
            SERIAL_ECHOPGM("point #");
            MYSERIAL.print(int(i));
            SERIAL_ECHOPGM("measured: (");
            MYSERIAL.print(measured_pts[i * 2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(measured_pts[i * 2 + 1], 5);
            SERIAL_ECHOPGM("); measured-corrected: (");
            MYSERIAL.print(x, 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(y, 5);
            SERIAL_ECHOPGM("); target: (");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2), 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2 + 1), 5);
            SERIAL_ECHOPGM("), error: ");
            MYSERIAL.print(sqrt(sqr(measured_pts[i * 2] - x) + sqr(measured_pts[i * 2 + 1] - y)));
            SERIAL_ECHOLNPGM("");
        }
        delay_keep_alive(100);
    }

    return result;
}

#endif

void reset_bed_offset_and_skew()
{
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_CENTER+0), 0x0FFFFFFFF);
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_CENTER+4), 0x0FFFFFFFF);
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_VEC_X +0), 0x0FFFFFFFF);
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_VEC_X +4), 0x0FFFFFFFF);
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_VEC_Y +0), 0x0FFFFFFFF);
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_VEC_Y +4), 0x0FFFFFFFF);

    // Reset the 8 16bit offsets.
    for (int8_t i = 0; i < 4; ++ i)
        eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_Z_JITTER+i*4), 0x0FFFFFFFF);
}

bool is_bed_z_jitter_data_valid()
{
    for (int8_t i = 0; i < 8; ++ i)
        if (eeprom_read_word((uint16_t*)(EEPROM_BED_CALIBRATION_Z_JITTER+i*2)) == 0x0FFFF)
            return false;
    return true;
}

static void world2machine_update(const float vec_x[2], const float vec_y[2], const float cntr[2])
{
    world2machine_rotation_and_skew[0][0] = vec_x[0];
    world2machine_rotation_and_skew[1][0] = vec_x[1];
    world2machine_rotation_and_skew[0][1] = vec_y[0];
    world2machine_rotation_and_skew[1][1] = vec_y[1];
    world2machine_shift[0] = cntr[0];
    world2machine_shift[1] = cntr[1];
    // No correction.
    world2machine_correction_mode = WORLD2MACHINE_CORRECTION_NONE;
    if (world2machine_shift[0] != 0.f || world2machine_shift[1] != 0.f)
        // Shift correction.
        world2machine_correction_mode |= WORLD2MACHINE_CORRECTION_SHIFT;
    if (world2machine_rotation_and_skew[0][0] != 1.f || world2machine_rotation_and_skew[0][1] != 0.f ||
        world2machine_rotation_and_skew[1][0] != 0.f || world2machine_rotation_and_skew[1][1] != 1.f) {
        // Rotation & skew correction.
        world2machine_correction_mode |= WORLD2MACHINE_CORRECTION_SKEW;
        // Invert the world2machine matrix.
        float d = world2machine_rotation_and_skew[0][0] * world2machine_rotation_and_skew[1][1] - world2machine_rotation_and_skew[1][0] * world2machine_rotation_and_skew[0][1];
        world2machine_rotation_and_skew_inv[0][0] =  world2machine_rotation_and_skew[1][1] / d;
        world2machine_rotation_and_skew_inv[0][1] = -world2machine_rotation_and_skew[0][1] / d;
        world2machine_rotation_and_skew_inv[1][0] = -world2machine_rotation_and_skew[1][0] / d;
        world2machine_rotation_and_skew_inv[1][1] =  world2machine_rotation_and_skew[0][0] / d;
    } else {
        world2machine_rotation_and_skew_inv[0][0] = 1.f;
        world2machine_rotation_and_skew_inv[0][1] = 0.f;
        world2machine_rotation_and_skew_inv[1][0] = 0.f;
        world2machine_rotation_and_skew_inv[1][1] = 1.f;
    }
}

void world2machine_reset()
{
    const float vx[] = { 1.f, 0.f };
    const float vy[] = { 0.f, 1.f };
    const float cntr[] = { 0.f, 0.f };
    world2machine_update(vx, vy, cntr);
}

static inline bool vec_undef(const float v[2])
{
    const uint32_t *vx = (const uint32_t*)v;
    return vx[0] == 0x0FFFFFFFF || vx[1] == 0x0FFFFFFFF;
}

void world2machine_initialize()
{
    SERIAL_ECHOLNPGM("world2machine_initialize()");
    float cntr[2] = {
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_CENTER+0)),
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_CENTER+4))
    };
    float vec_x[2] = {
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +0)),
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +4))
    };
    float vec_y[2] = {
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +0)),
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +4))
    };

    bool reset = false;
    if (vec_undef(cntr) || vec_undef(vec_x) || vec_undef(vec_y)) {
        SERIAL_ECHOLNPGM("Undefined bed correction matrix.");
        reset = true;
    }
    else {
        // Length of the vec_x shall be close to unity.
        float l = sqrt(vec_x[0] * vec_x[0] + vec_x[1] * vec_x[1]);
        if (l < 0.9 || l > 1.1) {
            SERIAL_ECHOLNPGM("Invalid bed correction matrix. Length of the X vector out of range.");
            reset = true;
        }
        // Length of the vec_y shall be close to unity.
        l = sqrt(vec_y[0] * vec_y[0] + vec_y[1] * vec_y[1]);
        if (l < 0.9 || l > 1.1) {
            SERIAL_ECHOLNPGM("Invalid bed correction matrix. Length of the X vector out of range.");
            reset = true;
        }
        // Correction of the zero point shall be reasonably small.
        l = sqrt(cntr[0] * cntr[0] + cntr[1] * cntr[1]);
        if (l > 15.f) {
            SERIAL_ECHOLNPGM("Invalid bed correction matrix. Shift out of range.");
            reset = true;
        }
        // vec_x and vec_y shall be nearly perpendicular.
        l = vec_x[0] * vec_y[0] + vec_x[1] * vec_y[1];
        if (fabs(l) > 0.1f) {
            SERIAL_ECHOLNPGM("Invalid bed correction matrix. X/Y axes are far from being perpendicular.");
            reset = true;
        }
    }

    if (reset) {
        SERIAL_ECHOLNPGM("Invalid bed correction matrix. Resetting to identity.");
        reset_bed_offset_and_skew();
        world2machine_reset();
    } else {
        world2machine_update(vec_x, vec_y, cntr);
        SERIAL_ECHOPGM("world2machine_initialize() loaded: ");
        MYSERIAL.print(world2machine_rotation_and_skew[0][0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(world2machine_rotation_and_skew[0][1], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(world2machine_rotation_and_skew[1][0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(world2machine_rotation_and_skew[1][1], 5);
        SERIAL_ECHOPGM(", offset ");
        MYSERIAL.print(world2machine_shift[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(world2machine_shift[1], 5);
        SERIAL_ECHOLNPGM("");
    }
}

// When switching from absolute to corrected coordinates,
// this will get the absolute coordinates from the servos,
// applies the inverse world2machine transformation
// and stores the result into current_position[x,y].
void world2machine_update_current()
{
    float x = current_position[X_AXIS] - world2machine_shift[0];
    float y = current_position[Y_AXIS] - world2machine_shift[1];
    current_position[X_AXIS] = world2machine_rotation_and_skew_inv[0][0] * x + world2machine_rotation_and_skew_inv[0][1] * y;
    current_position[Y_AXIS] = world2machine_rotation_and_skew_inv[1][0] * x + world2machine_rotation_and_skew_inv[1][1] * y;
}

static inline void go_xyz(float x, float y, float z, float fr)
{
    plan_buffer_line(x, y, z, current_position[E_AXIS], fr, active_extruder);
    st_synchronize();
}

static inline void go_xy(float x, float y, float fr)
{
    plan_buffer_line(x, y, current_position[Z_AXIS], current_position[E_AXIS], fr, active_extruder);
    st_synchronize();
}

static inline void go_to_current(float fr)
{
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], fr, active_extruder);
    st_synchronize();
}

static inline void update_current_position_xyz()
{
      current_position[X_AXIS] = st_get_position_mm(X_AXIS);
      current_position[Y_AXIS] = st_get_position_mm(Y_AXIS);
      current_position[Z_AXIS] = st_get_position_mm(Z_AXIS);
      plan_set_position(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS]);
}

static inline void update_current_position_z()
{
      current_position[Z_AXIS] = st_get_position_mm(Z_AXIS);
      plan_set_z_position(current_position[Z_AXIS]);
}

// At the current position, find the Z stop.
inline bool find_bed_induction_sensor_point_z(float minimum_z) 
{
    bool endstops_enabled  = enable_endstops(true);
    bool endstop_z_enabled = enable_z_endstop(false);
    endstop_z_hit_on_purpose();

    // move down until you find the bed
    current_position[Z_AXIS] = minimum_z;
    go_to_current(homing_feedrate[Z_AXIS]/60);
    // we have to let the planner know where we are right now as it is not where we said to go.
    update_current_position_z();
    if (! endstop_z_hit_on_purpose())
        goto error;

    // move up the retract distance
    current_position[Z_AXIS] += home_retract_mm_ext(Z_AXIS);
    go_to_current(homing_feedrate[Z_AXIS]/60);
    
    // move back down slowly to find bed
    current_position[Z_AXIS] -= home_retract_mm_ext(Z_AXIS) * 2;
    current_position[Z_AXIS] = min(current_position[Z_AXIS], minimum_z);
    go_to_current(homing_feedrate[Z_AXIS]/(4*60));
    // we have to let the planner know where we are right now as it is not where we said to go.
    update_current_position_z();
    if (! endstop_z_hit_on_purpose())
        goto error;

    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    return true;

error:
    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    return false;
}

// Search around the current_position[X,Y],
// look for the induction sensor response.
// Adjust the  current_position[X,Y,Z] to the center of the target dot and its response Z coordinate.
#define FIND_BED_INDUCTION_SENSOR_POINT_X_RADIUS (8.f)
#define FIND_BED_INDUCTION_SENSOR_POINT_Y_RADIUS (6.f)
#define FIND_BED_INDUCTION_SENSOR_POINT_XY_STEP  (1.f)
#define FIND_BED_INDUCTION_SENSOR_POINT_Z_STEP   (0.5f)
inline bool find_bed_induction_sensor_point_xy()
{
    float feedrate = homing_feedrate[X_AXIS] / 60.f;
    bool found = false;

    {
        float x0 = current_position[X_AXIS] - FIND_BED_INDUCTION_SENSOR_POINT_X_RADIUS;
        float x1 = current_position[X_AXIS] + FIND_BED_INDUCTION_SENSOR_POINT_X_RADIUS;
        float y0 = current_position[Y_AXIS] - FIND_BED_INDUCTION_SENSOR_POINT_Y_RADIUS;
        float y1 = current_position[Y_AXIS] + FIND_BED_INDUCTION_SENSOR_POINT_Y_RADIUS;
        uint8_t nsteps_y;
        uint8_t i;
        if (x0 < X_MIN_POS)
            x0 = X_MIN_POS;
        if (x1 > X_MAX_POS)
            x1 = X_MAX_POS;
        if (y0 < Y_MIN_POS_FOR_BED_CALIBRATION)
            y0 = Y_MIN_POS_FOR_BED_CALIBRATION;
        if (y1 > Y_MAX_POS)
            y1 = Y_MAX_POS;
        nsteps_y = int(ceil((y1 - y0) / FIND_BED_INDUCTION_SENSOR_POINT_XY_STEP));

        enable_endstops(false);
        bool  dir_positive = true;

//        go_xyz(current_position[X_AXIS], current_position[Y_AXIS], MESH_HOME_Z_SEARCH, homing_feedrate[Z_AXIS]/60);
        go_xyz(x0, y0, current_position[Z_AXIS], feedrate);
        // Continously lower the Z axis.
        endstops_hit_on_purpose();
        enable_z_endstop(true);
        while (current_position[Z_AXIS] > -10.f) {
            // Do nsteps_y zig-zag movements.
            current_position[Y_AXIS] = y0;
            for (i = 0; i < nsteps_y; current_position[Y_AXIS] += (y1 - y0) / float(nsteps_y - 1), ++ i) {
                // Run with a slightly decreasing Z axis, zig-zag movement. Stop at the Z end-stop.
                current_position[Z_AXIS] -= FIND_BED_INDUCTION_SENSOR_POINT_Z_STEP / float(nsteps_y);
                go_xyz(dir_positive ? x1 : x0, current_position[Y_AXIS], current_position[Z_AXIS], feedrate);
                dir_positive = ! dir_positive;
                if (endstop_z_hit_on_purpose())
                    goto endloop;
            }
            for (i = 0; i < nsteps_y; current_position[Y_AXIS] -= (y1 - y0) / float(nsteps_y - 1), ++ i) {
                // Run with a slightly decreasing Z axis, zig-zag movement. Stop at the Z end-stop.
                current_position[Z_AXIS] -= FIND_BED_INDUCTION_SENSOR_POINT_Z_STEP / float(nsteps_y);
                go_xyz(dir_positive ? x1 : x0, current_position[Y_AXIS], current_position[Z_AXIS], feedrate);
                dir_positive = ! dir_positive;
                if (endstop_z_hit_on_purpose())
                    goto endloop;
            }
        }
        endloop:
//        SERIAL_ECHOLN("First hit");

        // we have to let the planner know where we are right now as it is not where we said to go.
        update_current_position_xyz();

        // Search in this plane for the first hit. Zig-zag first in X, then in Y axis.
        for (int8_t iter = 0; iter < 3; ++ iter) {
            if (iter > 0) {
                // Slightly lower the Z axis to get a reliable trigger.
                current_position[Z_AXIS] -= 0.02f;
                go_xyz(current_position[X_AXIS], current_position[Y_AXIS], MESH_HOME_Z_SEARCH, homing_feedrate[Z_AXIS]/60);
            }

            // Do nsteps_y zig-zag movements.
            float a, b;
            enable_endstops(false);
            enable_z_endstop(false);
            current_position[Y_AXIS] = y0;
            go_xy(x0, current_position[Y_AXIS], feedrate);
            enable_z_endstop(true);
            found = false;
            for (i = 0, dir_positive = true; i < nsteps_y; current_position[Y_AXIS] += (y1 - y0) / float(nsteps_y - 1), ++ i, dir_positive = ! dir_positive) {
                go_xy(dir_positive ? x1 : x0, current_position[Y_AXIS], feedrate);
                if (endstop_z_hit_on_purpose()) {
                    found = true;
                    break;
                }
            }
            update_current_position_xyz();
            if (! found) {
//                SERIAL_ECHOLN("Search in Y - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search in Y - found");
            a = current_position[Y_AXIS];

            enable_z_endstop(false);
            current_position[Y_AXIS] = y1;
            go_xy(x0, current_position[Y_AXIS], feedrate);
            enable_z_endstop(true);
            found = false;
            for (i = 0, dir_positive = true; i < nsteps_y; current_position[Y_AXIS] -= (y1 - y0) / float(nsteps_y - 1), ++ i, dir_positive = ! dir_positive) {
                go_xy(dir_positive ? x1 : x0, current_position[Y_AXIS], feedrate);
                if (endstop_z_hit_on_purpose()) {
                    found = true;
                    break;
                }
            }
            update_current_position_xyz();
            if (! found) {
//                SERIAL_ECHOLN("Search in Y2 - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search in Y2 - found");
            b = current_position[Y_AXIS];
            current_position[Y_AXIS] = 0.5f * (a + b);

            // Search in the X direction along a cross.
            found = false;
            enable_z_endstop(false);
            go_xy(x0, current_position[Y_AXIS], feedrate);
            enable_z_endstop(true);
            go_xy(x1, current_position[Y_AXIS], feedrate);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
//                SERIAL_ECHOLN("Search X span 0 - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search X span 0 - found");
            a = current_position[X_AXIS];
            enable_z_endstop(false);
            go_xy(x1, current_position[Y_AXIS], feedrate);
            enable_z_endstop(true);
            go_xy(x0, current_position[Y_AXIS], feedrate);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
//                SERIAL_ECHOLN("Search X span 1 - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search X span 1 - found");
            b = current_position[X_AXIS];
            // Go to the center.
            enable_z_endstop(false);
            current_position[X_AXIS] = 0.5f * (a + b);
            go_xy(current_position[X_AXIS], current_position[Y_AXIS], feedrate);
            found = true;

#if 1
            // Search in the Y direction along a cross.
            found = false;
            enable_z_endstop(false);
            go_xy(current_position[X_AXIS], y0, feedrate);
            enable_z_endstop(true);
            go_xy(current_position[X_AXIS], y1, feedrate);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
//                SERIAL_ECHOLN("Search Y2 span 0 - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search Y2 span 0 - found");
            a = current_position[Y_AXIS];
            enable_z_endstop(false);
            go_xy(current_position[X_AXIS], y1, feedrate);
            enable_z_endstop(true);
            go_xy(current_position[X_AXIS], y0, feedrate);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
//                SERIAL_ECHOLN("Search Y2 span 1 - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search Y2 span 1 - found");
            b = current_position[Y_AXIS];
            // Go to the center.
            enable_z_endstop(false);
            current_position[Y_AXIS] = 0.5f * (a + b);
            go_xy(current_position[X_AXIS], current_position[Y_AXIS], feedrate);
            found = true;
#endif
            break;
        }
    }

    enable_z_endstop(false);
    return found;
}

// Search around the current_position[X,Y,Z].
// It is expected, that the induction sensor is switched on at the current position.
// Look around this center point by painting a star around the point.
inline bool improve_bed_induction_sensor_point()
{
    static const float search_radius = 8.f;

    bool  endstops_enabled  = enable_endstops(false);
    bool  endstop_z_enabled = enable_z_endstop(false);
    bool  found = false;
    float feedrate = homing_feedrate[X_AXIS] / 60.f;
    float center_old_x = current_position[X_AXIS];
    float center_old_y = current_position[Y_AXIS];
    float center_x = 0.f;
    float center_y = 0.f;

    for (uint8_t iter = 0; iter < 4; ++ iter) {
        switch (iter) {
        case 0:
            destination[X_AXIS] = center_old_x - search_radius * 0.707;
            destination[Y_AXIS] = center_old_y - search_radius * 0.707;
            break;
        case 1:
            destination[X_AXIS] = center_old_x + search_radius * 0.707;
            destination[Y_AXIS] = center_old_y + search_radius * 0.707;
            break;
        case 2:
            destination[X_AXIS] = center_old_x + search_radius * 0.707;
            destination[Y_AXIS] = center_old_y - search_radius * 0.707;
            break;
        case 3:
        default:
            destination[X_AXIS] = center_old_x - search_radius * 0.707;
            destination[Y_AXIS] = center_old_y + search_radius * 0.707;
            break;
        }

        // Trim the vector from center_old_[x,y] to destination[x,y] by the bed dimensions.
        float vx = destination[X_AXIS] - center_old_x;
        float vy = destination[Y_AXIS] - center_old_y;
        float l  = sqrt(vx*vx+vy*vy);
        float t;
        if (destination[X_AXIS] < X_MIN_POS) {
            // Exiting the bed at xmin.
            t = (center_x - X_MIN_POS) / l;
            destination[X_AXIS] = X_MIN_POS;
            destination[Y_AXIS] = center_old_y + t * vy;
        } else if (destination[X_AXIS] > X_MAX_POS) {
            // Exiting the bed at xmax.
            t = (X_MAX_POS - center_x) / l;
            destination[X_AXIS] = X_MAX_POS;
            destination[Y_AXIS] = center_old_y + t * vy;
        }
        if (destination[Y_AXIS] < Y_MIN_POS_FOR_BED_CALIBRATION) {
            // Exiting the bed at ymin.
            t = (center_y - Y_MIN_POS_FOR_BED_CALIBRATION) / l;
            destination[X_AXIS] = center_old_x + t * vx;
            destination[Y_AXIS] = Y_MIN_POS_FOR_BED_CALIBRATION;
        } else if (destination[Y_AXIS] > Y_MAX_POS) {
            // Exiting the bed at xmax.
            t = (Y_MAX_POS - center_y) / l;
            destination[X_AXIS] = center_old_x + t * vx;
            destination[Y_AXIS] = Y_MAX_POS;
        }

        // Move away from the measurement point.
        enable_endstops(false);
        go_xy(destination[X_AXIS], destination[Y_AXIS], feedrate);
        // Move towards the measurement point, until the induction sensor triggers.
        enable_endstops(true);
        go_xy(center_old_x, center_old_y, feedrate);
        update_current_position_xyz();
//        if (! endstop_z_hit_on_purpose()) return false;
        center_x += current_position[X_AXIS];
        center_y += current_position[Y_AXIS];
    }

    // Calculate the new center, move to the new center.
    center_x /= 4.f;
    center_y /= 4.f;
    current_position[X_AXIS] = center_x;
    current_position[Y_AXIS] = center_y;
    enable_endstops(false);
    go_xy(current_position[X_AXIS], current_position[Y_AXIS], feedrate);

    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    return found;
}

static inline void debug_output_point(const char *type, const float &x, const float &y, const float &z)
{
    SERIAL_ECHOPGM("Measured ");
    SERIAL_ECHORPGM(type);
    SERIAL_ECHOPGM(" ");
    MYSERIAL.print(x, 5);
    SERIAL_ECHOPGM(", ");
    MYSERIAL.print(y, 5);
    SERIAL_ECHOPGM(", ");
    MYSERIAL.print(z, 5);
    SERIAL_ECHOLNPGM("");
}

// Search around the current_position[X,Y,Z].
// It is expected, that the induction sensor is switched on at the current position.
// Look around this center point by painting a star around the point.
#define IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS (8.f)
inline bool improve_bed_induction_sensor_point2(bool lift_z_on_min_y, int8_t verbosity_level)
{
    float center_old_x = current_position[X_AXIS];
    float center_old_y = current_position[Y_AXIS];
    float a, b;

    enable_endstops(false);

    {
        float x0 = center_old_x - IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS;
        float x1 = center_old_x + IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS;
        if (x0 < X_MIN_POS)
            x0 = X_MIN_POS;
        if (x1 > X_MAX_POS)
            x1 = X_MAX_POS;

        // Search in the X direction along a cross.
        enable_z_endstop(false);
        go_xy(x0, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(x1, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (! endstop_z_hit_on_purpose()) {
            current_position[X_AXIS] = center_old_x;
            goto canceled;
        }
        a = current_position[X_AXIS];
        enable_z_endstop(false);
        go_xy(x1, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(x0, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (! endstop_z_hit_on_purpose()) {
            current_position[X_AXIS] = center_old_x;
            goto canceled;
        }
        b = current_position[X_AXIS];
        if (verbosity_level >= 5) {
            debug_output_point(PSTR("left" ), a, current_position[Y_AXIS], current_position[Z_AXIS]);
            debug_output_point(PSTR("right"), b, current_position[Y_AXIS], current_position[Z_AXIS]);
        }

        // Go to the center.
        enable_z_endstop(false);
        current_position[X_AXIS] = 0.5f * (a + b);
        go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
    }

    {
        float y0 = center_old_y - IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS;
        float y1 = center_old_y + IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS;
        if (y0 < Y_MIN_POS_FOR_BED_CALIBRATION)
            y0 = Y_MIN_POS_FOR_BED_CALIBRATION;
        if (y1 > Y_MAX_POS)
            y1 = Y_MAX_POS;

        // Search in the Y direction along a cross.
        enable_z_endstop(false);
        go_xy(current_position[X_AXIS], y0, homing_feedrate[X_AXIS] / 60.f);
        if (lift_z_on_min_y) {
            // The first row of points are very close to the end stop.
            // Lift the sensor to disengage the trigger. This is necessary because of the sensor hysteresis.
            go_xyz(current_position[X_AXIS], y0, current_position[Z_AXIS]+1.5f, homing_feedrate[Z_AXIS] / 60.f);
            // and go back.
            go_xyz(current_position[X_AXIS], y0, current_position[Z_AXIS], homing_feedrate[Z_AXIS] / 60.f);
        }
        if (lift_z_on_min_y && (READ(Z_MIN_PIN) ^ Z_MIN_ENDSTOP_INVERTING) == 1) {
            // Already triggering before we started the move.
            // Shift the trigger point slightly outwards.
            // a = current_position[Y_AXIS] - 1.5f;
            a = current_position[Y_AXIS];
        } else {
            enable_z_endstop(true);
            go_xy(current_position[X_AXIS], y1, homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                current_position[Y_AXIS] = center_old_y;
                goto canceled;
            }
            a = current_position[Y_AXIS];
        }
        enable_z_endstop(false);
        go_xy(current_position[X_AXIS], y1, homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(current_position[X_AXIS], y0, homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (! endstop_z_hit_on_purpose()) {
            current_position[Y_AXIS] = center_old_y;
            goto canceled;
        }
        b = current_position[Y_AXIS];
        if (verbosity_level >= 5) {
            debug_output_point(PSTR("top" ), current_position[X_AXIS], a, current_position[Z_AXIS]);
            debug_output_point(PSTR("bottom"), current_position[X_AXIS], b, current_position[Z_AXIS]);
        }

        // Go to the center.
        enable_z_endstop(false);
        current_position[Y_AXIS] = 0.5f * (a + b);
        go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
    }

    return true;

canceled:
    // Go back to the center.
    enable_z_endstop(false);
    go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
    return false;
}

// Searching the front points, where one cannot move the sensor head in front of the sensor point.
// Searching in a zig-zag movement in a plane for the maximum width of the response.
#define IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS (4.f)
#define IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y (0.1f)
enum InductionSensorPointStatusType
{
    INDUCTION_SENSOR_POINT_FAILED = -1,
    INDUCTION_SENSOR_POINT_OK = 0,
    INDUCTION_SENSOR_POINT_FAR,
};
inline InductionSensorPointStatusType improve_bed_induction_sensor_point3(int verbosity_level)
{
    float center_old_x = current_position[X_AXIS];
    float center_old_y = current_position[Y_AXIS];
    float a, b;
    // Was the sensor point detected too far in the minus Y axis?
    // If yes, the center of the induction point cannot be reached by the machine.
    bool y_too_far = false;
    {
        float x0 = center_old_x - IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
        float x1 = center_old_x + IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
        float y0 = center_old_y - IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
        float y1 = center_old_y + IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
        float y = y0;

        if (x0 < X_MIN_POS)
            x0 = X_MIN_POS;
        if (x1 > X_MAX_POS)
            x1 = X_MAX_POS;
        if (y0 < Y_MIN_POS_FOR_BED_CALIBRATION)
            y0 = Y_MIN_POS_FOR_BED_CALIBRATION;
        if (y1 > Y_MAX_POS)
            y1 = Y_MAX_POS;

        if (verbosity_level >= 20) {
            SERIAL_ECHOPGM("Initial position: ");
            SERIAL_ECHO(center_old_x);
            SERIAL_ECHOPGM(", ");
            SERIAL_ECHO(center_old_y);
            SERIAL_ECHOLNPGM("");
        }

        // Search in the positive Y direction, until a maximum diameter is found.
        float dmax = 0.f;
        float xmax1 = 0.f;
        float xmax2 = 0.f;
        for (float y = y0; y < y1; y += IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y) {
            enable_z_endstop(false);
            go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
            enable_z_endstop(true);
            go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                continue;
                // SERIAL_PROTOCOLPGM("Failed 1\n");
                // current_position[X_AXIS] = center_old_x;
                // goto canceled;
            }
            a = current_position[X_AXIS];
            enable_z_endstop(false);
            go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
            enable_z_endstop(true);
            go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                continue;
                // SERIAL_PROTOCOLPGM("Failed 2\n");
                // current_position[X_AXIS] = center_old_x;
                // goto canceled;
            }
            b = current_position[X_AXIS];
            if (verbosity_level >= 5) {
                debug_output_point(PSTR("left" ), a, current_position[Y_AXIS], current_position[Z_AXIS]);
                debug_output_point(PSTR("right"), b, current_position[Y_AXIS], current_position[Z_AXIS]);
            }
            float d = b - a;
            if (d > dmax) {
                xmax1 = 0.5f * (a + b);
                dmax = d;
            } else if (dmax > 0.) {
                y0 = y - IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y;
                break;
            }
        }
        if (dmax == 0.) {
            SERIAL_PROTOCOLPGM("failed - not found\n");
            goto canceled;
        }

        // SERIAL_PROTOCOLPGM("ok 1\n");
        // Search in the negative Y direction, until a maximum diameter is found.
        dmax = 0.;
        if (y0 + 1.f < y1)
            y1 = y0 + 1.f;
        for (float y = y1; y >= y0; y -= IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y) {
            enable_z_endstop(false);
            go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
            enable_z_endstop(true);
            go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                continue;
                /*
                current_position[X_AXIS] = center_old_x;
                SERIAL_PROTOCOLPGM("Failed 3\n");
                goto canceled;
                */
            }
            a = current_position[X_AXIS];
            enable_z_endstop(false);
            go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
            enable_z_endstop(true);
            go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                continue;
                /*
                current_position[X_AXIS] = center_old_x;
                SERIAL_PROTOCOLPGM("Failed 4\n");
                goto canceled;
                */
            }
            b = current_position[X_AXIS];
            if (verbosity_level >= 5) {
                debug_output_point(PSTR("left" ), a, current_position[Y_AXIS], current_position[Z_AXIS]);
                debug_output_point(PSTR("right"), b, current_position[Y_AXIS], current_position[Z_AXIS]);
            }
            float d = b - a;
            if (d > dmax) {
                xmax2 = 0.5f * (a + b);
                dmax = d;
            } else if (dmax > 0.) {
                y1 = y + IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y;
                break;
            }
        }
        // SERIAL_PROTOCOLPGM("ok 2\n");
        // Go to the center.
        enable_z_endstop(false);
        if (dmax == 0.f) {
            // Found only the point going from ymin to ymax.
            current_position[X_AXIS] = xmax1;
            current_position[Y_AXIS] = y0;
            y_too_far = true;
        } else {
            // Both points found (from ymin to ymax and from ymax to ymin).
            float p = 0.5f;
            // If the first hit was on the machine boundary,
            // give it a higher weight.
            if (y0 == Y_MIN_POS_FOR_BED_CALIBRATION)
                p = 0.75f;
            current_position[X_AXIS] = p * xmax1 + (1.f - p) * xmax2;
            current_position[Y_AXIS] = p * y0 + (1.f - p) * y1;
        }
        if (verbosity_level >= 20) {
            SERIAL_ECHOPGM("Adjusted position: ");
            SERIAL_ECHO(current_position[X_AXIS]);
            SERIAL_ECHOPGM(", ");
            SERIAL_ECHO(current_position[Y_AXIS]);
            SERIAL_ECHOLNPGM("");
        }
        go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        // delay_keep_alive(3000);
    }

    // Try yet to improve the X position.
    {
        float x0 = current_position[X_AXIS] - IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS;
        float x1 = current_position[X_AXIS] + IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS;
        if (x0 < X_MIN_POS)
            x0 = X_MIN_POS;
        if (x1 > X_MAX_POS)
            x1 = X_MAX_POS;

        // Search in the X direction along a cross.
        enable_z_endstop(false);
        go_xy(x0, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(x1, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (! endstop_z_hit_on_purpose()) {
            current_position[X_AXIS] = center_old_x;
            goto canceled;
        }
        a = current_position[X_AXIS];
        enable_z_endstop(false);
        go_xy(x1, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(x0, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (! endstop_z_hit_on_purpose()) {
            current_position[X_AXIS] = center_old_x;
            goto canceled;
        }
        b = current_position[X_AXIS];
        if (verbosity_level >= 5) {
            debug_output_point(PSTR("left" ), a, current_position[Y_AXIS], current_position[Z_AXIS]);
            debug_output_point(PSTR("right"), b, current_position[Y_AXIS], current_position[Z_AXIS]);
        }

        // Go to the center.
        enable_z_endstop(false);
        current_position[X_AXIS] = 0.5f * (a + b);
        go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
    }

    return y_too_far ? INDUCTION_SENSOR_POINT_FAR : INDUCTION_SENSOR_POINT_OK;

canceled:
    // Go back to the center.
    enable_z_endstop(false);
    go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
    return INDUCTION_SENSOR_POINT_FAILED;
}

// Scan the mesh bed induction points one by one by a left-right zig-zag movement,
// write the trigger coordinates to the serial line.
// Useful for visualizing the behavior of the bed induction detector.
inline void scan_bed_induction_sensor_point()
{
    float center_old_x = current_position[X_AXIS];
    float center_old_y = current_position[Y_AXIS];
    float x0 = center_old_x - IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
    float x1 = center_old_x + IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
    float y0 = center_old_y - IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
    float y1 = center_old_y + IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
    float y = y0;

    if (x0 < X_MIN_POS)
        x0 = X_MIN_POS;
    if (x1 > X_MAX_POS)
        x1 = X_MAX_POS;
    if (y0 < Y_MIN_POS_FOR_BED_CALIBRATION)
        y0 = Y_MIN_POS_FOR_BED_CALIBRATION;
    if (y1 > Y_MAX_POS)
        y1 = Y_MAX_POS;

    for (float y = y0; y < y1; y += IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y) {
        enable_z_endstop(false);
        go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (endstop_z_hit_on_purpose())
            debug_output_point(PSTR("left" ), current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS]);
        enable_z_endstop(false);
        go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (endstop_z_hit_on_purpose())
            debug_output_point(PSTR("right"), current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS]);
    }

    enable_z_endstop(false);
    current_position[X_AXIS] = center_old_x;
    current_position[Y_AXIS] = center_old_y;
    go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
}

#define MESH_BED_CALIBRATION_SHOW_LCD

BedSkewOffsetDetectionResultType find_bed_offset_and_skew(int8_t verbosity_level)
{
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    // Reusing the z_values memory for the measurement cache.
    // 7x7=49 floats, good for 16 (x,y,z) vectors.
    float *pts = &mbl.z_values[0][0];
    float *vec_x = pts + 2 * 4;
    float *vec_y = vec_x + 2;
    float *cntr  = vec_y + 2;
    memset(pts, 0, sizeof(float) * 7 * 7);

//    SERIAL_ECHOLNPGM("find_bed_offset_and_skew verbosity level: ");
//    SERIAL_ECHO(int(verbosity_level));
//    SERIAL_ECHOPGM("");

#ifdef MESH_BED_CALIBRATION_SHOW_LCD
    lcd_implementation_clear();
    lcd_print_at_PGM(0, 0, MSG_FIND_BED_OFFSET_AND_SKEW_LINE1);
#endif /* MESH_BED_CALIBRATION_SHOW_LCD */

    // Collect the rear 2x3 points.
    current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
    for (int k = 0; k < 4; ++ k) {
        // Don't let the manage_inactivity() function remove power from the motors.
        refresh_cmd_timeout();
#ifdef MESH_BED_CALIBRATION_SHOW_LCD
        lcd_print_at_PGM(0, 1, MSG_FIND_BED_OFFSET_AND_SKEW_LINE2);
        lcd_implementation_print_at(0, 2, k+1);
        lcd_printPGM(MSG_FIND_BED_OFFSET_AND_SKEW_LINE3);
#endif /* MESH_BED_CALIBRATION_SHOW_LCD */
        float *pt = pts + k * 2;
        // Go up to z_initial.
        go_to_current(homing_feedrate[Z_AXIS] / 60.f);
        if (verbosity_level >= 20) {
            // Go to Y0, wait, then go to Y-4.
            current_position[Y_AXIS] = 0.f;
            go_to_current(homing_feedrate[X_AXIS] / 60.f);
            SERIAL_ECHOLNPGM("At Y0");
            delay_keep_alive(5000);
            current_position[Y_AXIS] = Y_MIN_POS;
            go_to_current(homing_feedrate[X_AXIS] / 60.f);
            SERIAL_ECHOLNPGM("At Y-4");
            delay_keep_alive(5000);
        }
        // Go to the measurement point position.
        current_position[X_AXIS] = pgm_read_float(bed_ref_points_4+k*2);
        current_position[Y_AXIS] = pgm_read_float(bed_ref_points_4+k*2+1);
        go_to_current(homing_feedrate[X_AXIS] / 60.f);
        if (verbosity_level >= 10)
            delay_keep_alive(3000);
        if (! find_bed_induction_sensor_point_xy())
            return BED_SKEW_OFFSET_DETECTION_FAILED;
        find_bed_induction_sensor_point_z();
#if 1
        if (k == 0) {
            // Improve the position of the 1st row sensor points by a zig-zag movement.
            int8_t i = 4;
            for (;;) {
                if (improve_bed_induction_sensor_point3(verbosity_level) != INDUCTION_SENSOR_POINT_FAILED)
                    break;
                if (-- i == 0)
                    return BED_SKEW_OFFSET_DETECTION_FAILED;
                // Try to move the Z axis down a bit to increase a chance of the sensor to trigger.
                current_position[Z_AXIS] -= 0.025f;
                enable_endstops(false);
                enable_z_endstop(false);
                go_to_current(homing_feedrate[Z_AXIS]);
            }
            if (i == 0)
                // not found
                return BED_SKEW_OFFSET_DETECTION_FAILED;
        }
#endif
        if (verbosity_level >= 10)
            delay_keep_alive(3000);
        pt[0] = current_position[X_AXIS];
        pt[1] = current_position[Y_AXIS];
        // Start searching for the other points at 3mm above the last point.
        current_position[Z_AXIS] += 3.f;
        cntr[0] += pt[0];
        cntr[1] += pt[1];
        if (verbosity_level >= 10 && k == 0) {
            // Show the zero. Test, whether the Y motor skipped steps.
            current_position[Y_AXIS] = MANUAL_Y_HOME_POS;
            go_to_current(homing_feedrate[X_AXIS] / 60.f);
            delay_keep_alive(3000);
        }
    }

    if (verbosity_level >= 20) {
        // Test the positions. Are the positions reproducible? Now the calibration is active in the planner.
        delay_keep_alive(3000);
        for (int8_t mesh_point = 0; mesh_point < 4; ++ mesh_point) {
            // Don't let the manage_inactivity() function remove power from the motors.
            refresh_cmd_timeout();
            // Go to the measurement point.
            // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
            current_position[X_AXIS] = pts[mesh_point*2];
            current_position[Y_AXIS] = pts[mesh_point*2+1];
            go_to_current(homing_feedrate[X_AXIS]/60);
            delay_keep_alive(3000);
        }
    }

    calculate_machine_skew_and_offset_LS(pts, 4, bed_ref_points_4, vec_x, vec_y, cntr, verbosity_level);
    world2machine_update(vec_x, vec_y, cntr);
#if 1
    // Fearlessly store the calibration values into the eeprom.
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_CENTER+0), cntr [0]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_CENTER+4), cntr [1]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +0), vec_x[0]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +4), vec_x[1]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +0), vec_y[0]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +4), vec_y[1]);
#endif

    // Correct the current_position to match the transformed coordinate system after world2machine_rotation_and_skew and world2machine_shift were set.
    world2machine_update_current();

    if (verbosity_level >= 20) {
        // Test the positions. Are the positions reproducible? Now the calibration is active in the planner.
        delay_keep_alive(3000);
        for (int8_t mesh_point = 0; mesh_point < 9; ++ mesh_point) {
            // Don't let the manage_inactivity() function remove power from the motors.
            refresh_cmd_timeout();
            // Go to the measurement point.
            // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
            current_position[X_AXIS] = pgm_read_float(bed_ref_points+mesh_point*2);
            current_position[Y_AXIS] = pgm_read_float(bed_ref_points+mesh_point*2+1);
            go_to_current(homing_feedrate[X_AXIS]/60);
            delay_keep_alive(3000);
        }
    }

    return BED_SKEW_OFFSET_DETECTION_PERFECT;
}

BedSkewOffsetDetectionResultType improve_bed_offset_and_skew(int8_t method, int8_t verbosity_level)
{
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    // Reusing the z_values memory for the measurement cache.
    // 7x7=49 floats, good for 16 (x,y,z) vectors.
    float *pts = &mbl.z_values[0][0];
    float *vec_x = pts + 2 * 9;
    float *vec_y = vec_x + 2;
    float *cntr  = vec_y + 2;
    memset(pts, 0, sizeof(float) * 7 * 7);

    // Cache the current correction matrix.
    world2machine_initialize();
    vec_x[0] = world2machine_rotation_and_skew[0][0];
    vec_x[1] = world2machine_rotation_and_skew[1][0];
    vec_y[0] = world2machine_rotation_and_skew[0][1];
    vec_y[1] = world2machine_rotation_and_skew[1][1];
    cntr[0] = world2machine_shift[0];
    cntr[1] = world2machine_shift[1];
    // and reset the correction matrix, so the planner will not do anything.
    world2machine_reset();

    bool endstops_enabled  = enable_endstops(false);
    bool endstop_z_enabled = enable_z_endstop(false);

#ifdef MESH_BED_CALIBRATION_SHOW_LCD
    lcd_implementation_clear();
    lcd_print_at_PGM(0, 0, MSG_IMPROVE_BED_OFFSET_AND_SKEW_LINE1);
#endif /* MESH_BED_CALIBRATION_SHOW_LCD */

    // Collect a matrix of 9x9 points.
    bool leftFrontTooFar = false;
    bool rightFrontTooFar = false;
    BedSkewOffsetDetectionResultType result = BED_SKEW_OFFSET_DETECTION_PERFECT;
    for (int8_t mesh_point = 0; mesh_point < 9; ++ mesh_point) {
        // Don't let the manage_inactivity() function remove power from the motors.
        refresh_cmd_timeout();
        // Print the decrasing ID of the measurement point.
#ifdef MESH_BED_CALIBRATION_SHOW_LCD
        lcd_print_at_PGM(0, 1, MSG_IMPROVE_BED_OFFSET_AND_SKEW_LINE2);
        lcd_implementation_print_at(0, 2, mesh_point+1);
        lcd_printPGM(MSG_IMPROVE_BED_OFFSET_AND_SKEW_LINE3);
#endif /* MESH_BED_CALIBRATION_SHOW_LCD */

        // Move up.
        current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
        enable_endstops(false);
        enable_z_endstop(false);
        go_to_current(homing_feedrate[Z_AXIS]/60);
        if (verbosity_level >= 20) {
            // Go to Y0, wait, then go to Y-4.
            current_position[Y_AXIS] = 0.f;
            go_to_current(homing_feedrate[X_AXIS] / 60.f);
            SERIAL_ECHOLNPGM("At Y0");
            delay_keep_alive(5000);
            current_position[Y_AXIS] = Y_MIN_POS;
            go_to_current(homing_feedrate[X_AXIS] / 60.f);
            SERIAL_ECHOLNPGM("At Y-4");
            delay_keep_alive(5000);
        }
        // Go to the measurement point.
        // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
        current_position[X_AXIS] = vec_x[0] * pgm_read_float(bed_ref_points+mesh_point*2) + vec_y[0] * pgm_read_float(bed_ref_points+mesh_point*2+1) + cntr[0];
        current_position[Y_AXIS] = vec_x[1] * pgm_read_float(bed_ref_points+mesh_point*2) + vec_y[1] * pgm_read_float(bed_ref_points+mesh_point*2+1) + cntr[1];
        // The calibration points are very close to the min Y.
        if (current_position[Y_AXIS] < Y_MIN_POS_FOR_BED_CALIBRATION)
            current_position[Y_AXIS] = Y_MIN_POS_FOR_BED_CALIBRATION;
        go_to_current(homing_feedrate[X_AXIS]/60);
        // Find its Z position by running the normal vertical search.
        if (verbosity_level >= 10)
            delay_keep_alive(3000);
        find_bed_induction_sensor_point_z();
        if (verbosity_level >= 10)
            delay_keep_alive(3000);
        // Improve the point position by searching its center in a current plane.
        int8_t n_errors = 3;
        for (int8_t iter = 0; iter < 8; ) {
            if (verbosity_level > 20) {
                SERIAL_ECHOPGM("Improving bed point ");
                SERIAL_ECHO(mesh_point);
                SERIAL_ECHOPGM(", iteration ");
                SERIAL_ECHO(iter);
                SERIAL_ECHOPGM(", z");
                MYSERIAL.print(current_position[Z_AXIS], 5);
                SERIAL_ECHOLNPGM("");
            }
            bool found = false;
            if (mesh_point < 3) {
                // Because the sensor cannot move in front of the first row
                // of the sensor points, the y position cannot be measured
                // by a cross center method.
                // Use a zig-zag search for the first row of the points.
                InductionSensorPointStatusType status = improve_bed_induction_sensor_point3(verbosity_level);
                if (status == INDUCTION_SENSOR_POINT_FAILED) {
                    found = false;
                } else {
                    found = true;
                    if (iter == 7 && INDUCTION_SENSOR_POINT_FAR && mesh_point != 1)
                        // Remember, which side of the bed is shifted too far in the minus y direction.
                        result = (mesh_point == 0) ? BED_SKEW_OFFSET_DETECTION_FRONT_LEFT_FAR : BED_SKEW_OFFSET_DETECTION_FRONT_RIGHT_FAR;
                }
            } else {
                switch (method) {
                    case 0: found = improve_bed_induction_sensor_point(); break;
                    case 1: found = improve_bed_induction_sensor_point2(mesh_point < 3, verbosity_level); break;
                    default: break;
                }
            }
            if (found) {
                if (iter > 3) {
                    // Average the last 4 measurements.
                    pts[mesh_point*2  ] += current_position[X_AXIS];
                    pts[mesh_point*2+1] += current_position[Y_AXIS];
                }
                ++ iter;
            } else if (n_errors -- == 0) {
                // Give up.
                goto canceled;
            } else {
                // Try to move the Z axis down a bit to increase a chance of the sensor to trigger.
                current_position[Z_AXIS] -= 0.025f;
                enable_endstops(false);
                enable_z_endstop(false);
                go_to_current(homing_feedrate[Z_AXIS]);
                if (verbosity_level > 20) {
                    SERIAL_ECHOPGM("Improving bed point ");
                    SERIAL_ECHO(mesh_point);
                    SERIAL_ECHOPGM(", iteration ");
                    SERIAL_ECHO(iter);
                    SERIAL_ECHOPGM(" failed. Lowering z to ");
                    MYSERIAL.print(current_position[Z_AXIS], 5);
                    SERIAL_ECHOLNPGM("");
                }
            }
        }
        if (verbosity_level >= 10)
            delay_keep_alive(3000);
    }
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    // Average the last 4 measurements.
    for (int8_t i = 0; i < 18; ++ i)
        pts[i] *= (1.f/4.f);

    enable_endstops(false);
    enable_z_endstop(false);

    if (verbosity_level >= 5) {
        // Test the positions. Are the positions reproducible?
        for (int8_t mesh_point = 0; mesh_point < 9; ++ mesh_point) {
            // Don't let the manage_inactivity() function remove power from the motors.
            refresh_cmd_timeout();
            // Go to the measurement point.
            // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
            current_position[X_AXIS] = pts[mesh_point*2];
            current_position[Y_AXIS] = pts[mesh_point*2+1];
            if (verbosity_level >= 10) {
                go_to_current(homing_feedrate[X_AXIS]/60);
                delay_keep_alive(3000);
            }
            SERIAL_ECHOPGM("Final measured bed point ");
            SERIAL_ECHO(mesh_point);
            SERIAL_ECHOPGM(": ");
            MYSERIAL.print(current_position[X_AXIS], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(current_position[Y_AXIS], 5);
            SERIAL_ECHOLNPGM("");
        }
    }

    result = calculate_machine_skew_and_offset_LS(pts, 9, bed_ref_points, vec_x, vec_y, cntr, verbosity_level);
    if (result == BED_SKEW_OFFSET_DETECTION_FAILED)
        goto canceled;

    world2machine_update(vec_x, vec_y, cntr);
#if 1
    // Fearlessly store the calibration values into the eeprom.
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_CENTER+0), cntr [0]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_CENTER+4), cntr [1]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +0), vec_x[0]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +4), vec_x[1]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +0), vec_y[0]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +4), vec_y[1]);
#endif

    // Correct the current_position to match the transformed coordinate system after world2machine_rotation_and_skew and world2machine_shift were set.
    world2machine_update_current();

    enable_endstops(false);
    enable_z_endstop(false);

    if (verbosity_level >= 5) {
        // Test the positions. Are the positions reproducible? Now the calibration is active in the planner.
        delay_keep_alive(3000);
        for (int8_t mesh_point = 0; mesh_point < 9; ++ mesh_point) {
            // Don't let the manage_inactivity() function remove power from the motors.
            refresh_cmd_timeout();
            // Go to the measurement point.
            // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
            current_position[X_AXIS] = pgm_read_float(bed_ref_points+mesh_point*2);
            current_position[Y_AXIS] = pgm_read_float(bed_ref_points+mesh_point*2+1);
            if (verbosity_level >= 10) {
                go_to_current(homing_feedrate[X_AXIS]/60);
                delay_keep_alive(3000);
            }
            SERIAL_ECHOPGM("Final calculated bed point ");
            SERIAL_ECHO(mesh_point);
            SERIAL_ECHOPGM(": ");
            MYSERIAL.print(st_get_position_mm(X_AXIS), 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(st_get_position_mm(Y_AXIS), 5);
            SERIAL_ECHOLNPGM("");
        }
    }

    // Sample Z heights for the mesh bed leveling.
    // In addition, store the results into an eeprom, to be used later for verification of the bed leveling process.
    {
        // The first point defines the reference.
        current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
        go_to_current(homing_feedrate[Z_AXIS]/60);
        current_position[X_AXIS] = pgm_read_float(bed_ref_points);
        current_position[Y_AXIS] = pgm_read_float(bed_ref_points+1);
        go_to_current(homing_feedrate[X_AXIS]/60);
        memcpy(destination, current_position, sizeof(destination));
        enable_endstops(true);
        homeaxis(Z_AXIS);
        mbl.set_z(0, 0, current_position[Z_AXIS]);
        enable_endstops(false);
    }
    for (int8_t mesh_point = 1; mesh_point != MESH_MEAS_NUM_X_POINTS * MESH_MEAS_NUM_Y_POINTS; ++ mesh_point) {
        current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
        go_to_current(homing_feedrate[Z_AXIS]/60);
        current_position[X_AXIS] = pgm_read_float(bed_ref_points+2*mesh_point);
        current_position[Y_AXIS] = pgm_read_float(bed_ref_points+2*mesh_point+1);
        go_to_current(homing_feedrate[X_AXIS]/60);
        find_bed_induction_sensor_point_z();
        // Get cords of measuring point
        int8_t ix = mesh_point % MESH_MEAS_NUM_X_POINTS;
        int8_t iy = mesh_point / MESH_MEAS_NUM_X_POINTS;
        if (iy & 1) ix = (MESH_MEAS_NUM_X_POINTS - 1) - ix; // Zig zag
        mbl.set_z(ix, iy, current_position[Z_AXIS]);
    }
    {
        // Verify the span of the Z values.
        float zmin = mbl.z_values[0][0];
        float zmax = zmax;
        for (int8_t j = 0; j < 3; ++ j)
           for (int8_t i = 0; i < 3; ++ i) {
                zmin = min(zmin, mbl.z_values[j][i]);
                zmax = min(zmax, mbl.z_values[j][i]);
           }
        if (zmax - zmin > 3.f) {
            // The span of the Z offsets is extreme. Give up.
            // Homing failed on some of the points.
            SERIAL_PROTOCOLLNPGM("Exreme span of the Z values!");
            goto canceled;
        }
    }

    // Store the correction values to EEPROM.
    // Offsets of the Z heiths of the calibration points from the first point.
    // The offsets are saved as 16bit signed int, scaled to tenths of microns.
    {
        uint16_t addr = EEPROM_BED_CALIBRATION_Z_JITTER;
        for (int8_t j = 0; j < 3; ++ j)
            for (int8_t i = 0; i < 3; ++ i) {
                if (i == 0 && j == 0)
                    continue;
                float dif = mbl.z_values[j][i] - mbl.z_values[0][0];
                int16_t dif_quantized = int16_t(floor(dif * 100.f + 0.5f));
                eeprom_update_word((uint16_t*)addr, *reinterpret_cast<uint16_t*>(&dif_quantized));
                {
                    uint16_t z_offset_u = eeprom_read_word((uint16_t*)addr);
                    float dif2 = *reinterpret_cast<int16_t*>(&z_offset_u) * 0.01;

                    SERIAL_ECHOPGM("Bed point ");
                    SERIAL_ECHO(i);
                    SERIAL_ECHOPGM(",");
                    SERIAL_ECHO(j);
                    SERIAL_ECHOPGM(", differences: written ");
                    MYSERIAL.print(dif, 5);
                    SERIAL_ECHOPGM(", read: ");
                    MYSERIAL.print(dif2, 5);
                    SERIAL_ECHOLNPGM("");
                }
                addr += 2;
            }
    }

    mbl.upsample_3x3();
    mbl.active = true;

    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    // Go home.
    current_position[Z_AXIS] = Z_MIN_POS;
    go_to_current(homing_feedrate[Z_AXIS]/60);
    current_position[X_AXIS] = X_MIN_POS+0.2;
    current_position[Y_AXIS] = Y_MIN_POS+0.2;
    go_to_current(homing_feedrate[X_AXIS]/60);

    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();
    return result;

canceled:
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();
    // Print head up.
    current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
    go_to_current(homing_feedrate[Z_AXIS]/60);
    // Store the identity matrix to EEPROM.
    reset_bed_offset_and_skew();
    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    return BED_SKEW_OFFSET_DETECTION_FAILED;
}

bool scan_bed_induction_points(int8_t verbosity_level)
{
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    // Reusing the z_values memory for the measurement cache.
    // 7x7=49 floats, good for 16 (x,y,z) vectors.
    float *pts = &mbl.z_values[0][0];
    float *vec_x = pts + 2 * 9;
    float *vec_y = vec_x + 2;
    float *cntr  = vec_y + 2;
    memset(pts, 0, sizeof(float) * 7 * 7);

    // Cache the current correction matrix.
    world2machine_initialize();
    vec_x[0] = world2machine_rotation_and_skew[0][0];
    vec_x[1] = world2machine_rotation_and_skew[1][0];
    vec_y[0] = world2machine_rotation_and_skew[0][1];
    vec_y[1] = world2machine_rotation_and_skew[1][1];
    cntr[0] = world2machine_shift[0];
    cntr[1] = world2machine_shift[1];
    // and reset the correction matrix, so the planner will not do anything.
    world2machine_reset();

    bool endstops_enabled  = enable_endstops(false);
    bool endstop_z_enabled = enable_z_endstop(false);

    // Collect a matrix of 9x9 points.
    for (int8_t mesh_point = 0; mesh_point < 9; ++ mesh_point) {
        // Don't let the manage_inactivity() function remove power from the motors.
        refresh_cmd_timeout();

        // Move up.
        current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
        enable_endstops(false);
        enable_z_endstop(false);
        go_to_current(homing_feedrate[Z_AXIS]/60);
        // Go to the measurement point.
        // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
        current_position[X_AXIS] = vec_x[0] * pgm_read_float(bed_ref_points+mesh_point*2) + vec_y[0] * pgm_read_float(bed_ref_points+mesh_point*2+1) + cntr[0];
        current_position[Y_AXIS] = vec_x[1] * pgm_read_float(bed_ref_points+mesh_point*2) + vec_y[1] * pgm_read_float(bed_ref_points+mesh_point*2+1) + cntr[1];
        // The calibration points are very close to the min Y.
        if (current_position[Y_AXIS] < Y_MIN_POS_FOR_BED_CALIBRATION)
            current_position[Y_AXIS] = Y_MIN_POS_FOR_BED_CALIBRATION;
        go_to_current(homing_feedrate[X_AXIS]/60);
        find_bed_induction_sensor_point_z();
        scan_bed_induction_sensor_point();
    }
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    enable_endstops(false);
    enable_z_endstop(false);

    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    return true;
}
