#include <mpi.h>
#include <omp.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <iostream>

bool read_matrix_from_file(const char *filename, double **matrix_out, size_t *num_rows_out, size_t *num_cols_out) {
    double *matrix;
    size_t num_rows;
    size_t num_cols;

    FILE *file = fopen(filename, "rb");
    if (file == nullptr) {
        fprintf(stderr, "Cannot open output file\n");
        return false;
    }

    fread(&num_rows, sizeof(size_t), 1, file);
    fread(&num_cols, sizeof(size_t), 1, file);
    matrix = new double[num_rows * num_cols];
    fread(matrix, sizeof(double), num_rows * num_cols, file);

    *matrix_out = matrix;
    *num_rows_out = num_rows;
    *num_cols_out = num_cols;

    fclose(file);

    return true;
}

bool write_matrix_to_file(const char *filename, const double *matrix, size_t num_rows, size_t num_cols) {
    FILE *file = fopen(filename, "wb");
    if (file == nullptr) {
        fprintf(stderr, "Cannot open output file\n");
        return false;
    }

    fwrite(&num_rows, sizeof(size_t), 1, file);
    fwrite(&num_cols, sizeof(size_t), 1, file);
    fwrite(matrix, sizeof(double), num_rows * num_cols, file);

    fclose(file);

    return true;
}

void print_matrix(const double *matrix, size_t num_rows, size_t num_cols, FILE *file = stdout) {
    fprintf(file, "%zu %zu\n", num_rows, num_cols);
    for (size_t r = 0; r < num_rows; r++) {
        for (size_t c = 0; c < num_cols; c++) {
            double val = matrix[r * num_cols + c];
            printf("%+6.3f ", val);
        }
        printf("\n");
    }
}

// serial function
double dot(const double *x, const double *y, size_t size) {
    double result = 0.0;
    for (size_t i = 0; i < size; i++) {
        result += x[i] * y[i];
    }
    return result;
}

void axpby(double alpha, const double *x, double beta, double *y, size_t size) {
    // y = alpha * x + beta * y
    for (size_t i = 0; i < size; i++) {
        y[i] = alpha * x[i] + beta * y[i];
    }
}

void axpbyP(double alpha, const double *x, double beta, double *y, size_t size) {
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < size; i++) {
        y[i] = alpha * x[i] + beta * y[i];
    }
}

void gemv(double alpha, const double *A, const double *x, double beta, double *y, size_t num_rows, size_t num_cols) {
    // y = alpha * A * x + beta * y;
    for (size_t r = 0; r < num_rows; r++) {
        double y_val = 0.0;
        for (size_t c = 0; c < num_cols; c++) {
            y_val += alpha * A[r * num_cols + c] * x[c];
        }
        y[r] = beta * y[r] + y_val;
    }
}

void gemvP(double alpha, const double *A, const double *x, double beta, double *y, size_t num_rows, size_t num_cols) {
    int rank, size_mpi;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size_mpi);

    size_t local_num_rows = num_rows / size_mpi;
    size_t start_row = rank * local_num_rows;
    size_t end_row = (rank + 1) * local_num_rows;
    if (rank == size_mpi - 1) end_row = num_rows;

    double *local_y = new double[local_num_rows];
    for (size_t r = start_row; r < end_row; r++) {
        double y_val = 0.0;
        #pragma omp parallel for reduction(+:y_val)
        for (size_t c = 0; c < num_cols; c++) {
            y_val += A[r * num_cols + c] * x[c];
        }
        local_y[r - start_row] = beta * y[r] + alpha * y_val;
    }

    MPI_Allgather(local_y, local_num_rows, MPI_DOUBLE, y, local_num_rows, MPI_DOUBLE, MPI_COMM_WORLD);

    delete[] local_y;
}

double dotP(const double *x, const double *y, size_t size) {
    int rank, size_mpi;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size_mpi);

    size_t local_size = size / size_mpi;
    size_t start = rank * local_size;
    size_t end = (rank == size_mpi - 1) ? size : start + local_size;

    double local_sum = 0.0;
    #pragma omp parallel for reduction(+:local_sum)
    for (size_t i = start; i < end; i++) {
        local_sum += x[i] * y[i];
    }

    double global_sum = 0.0;
    MPI_Allreduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    return global_sum;
}

void conjugate_gradients(const double *A, const double *b, double *x, size_t size, int max_iters, double rel_error) {
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    double alpha, beta, bb, rr, rr_new;
    double *r = new double[size];
    double *p = new double[size];
    double *Ap = new double[size];
    int num_iters;

    #pragma omp parallel for
    for (size_t i = 0; i < size; i++) {
        x[i] = 0.0;
        r[i] = b[i];
        p[i] = b[i];
    }

    bb = dotP(b, b, size);
    rr = bb;

    for (num_iters = 1; num_iters <= max_iters; num_iters++) {
        gemvP(1.0, A, p, 0.0, Ap, size, size);
        alpha = rr / dotP(p, Ap, size);
        axpbyP(alpha, p, 1.0, x, size);
        axpbyP(-alpha, Ap, 1.0, r, size);
        rr_new = dotP(r, r, size);
        beta = rr_new / rr;
        rr = rr_new;
        if (sqrt(rr / bb) < rel_error) {
            break;
        }
        axpbyP(1.0, r, beta, p, size);
    }

    delete[] r;
    delete[] p;
    delete[] Ap;

    if (rank == 0) {
        if (num_iters <= max_iters) {
            printf("Converged in %d iterations, relative error is %e\n", num_iters, sqrt(rr / bb));
        } else {
            printf("Did not converge in %d iterations, relative error is %e\n", max_iters, sqrt(rr / bb));
        }
    }
}

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, size_mpi;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size_mpi);

    const char *input_file_matrix = "io/matrix.bin";
    const char *input_file_rhs = "io/rhs.bin";
    const char *output_file_sol = "io/sol.bin";
    int max_iters = 1000;
    double rel_error = 1e-9;
    std::chrono::high_resolution_clock::time_point start, finish;

    if (argc > 1) input_file_matrix = argv[1];
    if (argc > 2) input_file_rhs = argv[2];
    if (argc > 3) output_file_sol = argv[3];
    if (argc > 4) max_iters = atoi(argv[4]);
    if (argc > 5) rel_error = atof(argv[5]);

    double *matrix;
    double *rhs;
    size_t size;
    size_t matrix_rows;
    size_t matrix_cols;
    size_t rhs_rows;
    size_t rhs_cols;

    if (rank == 0) {

        printf("Usage: ./random_matrix input_file_matrix.bin input_file_rhs.bin output_file_sol.bin max_iters rel_error\n");
        printf("All parameters are optional and have default values\n");
        printf("\n");

        printf("Command line arguments:\n");
        printf("  input_file_matrix: %s\n", input_file_matrix);
        printf("  input_file_rhs:    %s\n", input_file_rhs);
        printf("  output_file_sol:   %s\n", output_file_sol);
        printf("  max_iters:         %d\n", max_iters);
        printf("  rel_error:         %e\n", rel_error);
        printf("\n");

        printf("Reading matrix from file ...\n");
        bool success_read_matrix = read_matrix_from_file(input_file_matrix, &matrix, &matrix_rows, &matrix_cols);
        if (!success_read_matrix) {
            fprintf(stderr, "Failed to read matrix\n");
            return 1;
        }
        printf("Done\n");
        printf("\n");

        printf("Reading right hand side from file ...\n");
        bool success_read_rhs = read_matrix_from_file(input_file_rhs, &rhs, &rhs_rows, &rhs_cols);
        if (!success_read_rhs) {
            fprintf(stderr, "Failed to read right hand side\n");
            return 2;
        }
        printf("Done\n");
        printf("\n");

        if (matrix_rows != matrix_cols) {
            fprintf(stderr, "Matrix has to be square\n");
            return 3;
        }
        if (rhs_rows != matrix_rows) {
            fprintf(stderr, "Size of right hand side does not match the matrix\n");
            return 4;
        }
        if (rhs_cols != 1) {
            fprintf(stderr, "Right hand side has to have just a single column\n");
            return 5;
        }

        size = matrix_rows;
    }

    // Broadcast of matrix rows, cols and size
    MPI_Bcast(&matrix_rows, 1, MPI_UNSIGNED_LONG, 0, MPI_COMM_WORLD);
    MPI_Bcast(&matrix_cols, 1, MPI_UNSIGNED_LONG, 0, MPI_COMM_WORLD);
    MPI_Bcast(&size, 1, MPI_UNSIGNED_LONG, 0, MPI_COMM_WORLD);

    if (rank != 0) {
        // Allocates spaces for non-zero precesses
        matrix = new double[size * size];
        rhs = new double[size];
        start = std::chrono::high_resolution_clock::now();
    }

    MPI_Bcast(matrix, matrix_rows * matrix_cols, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Bcast(rhs, matrix_rows, MPI_DOUBLE, 0, MPI_COMM_WORLD);

    double *sol = new double[size];

    conjugate_gradients(matrix, rhs, sol, size, max_iters, rel_error);

    if (rank == 0) {

        finish = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> time = std::chrono::duration_cast<std::chrono::duration<double>>(finish-start);
        double cpu_time = time.count();
        std::cout << "Total CPU time = " << cpu_time << std::endl;

        printf("\n\nWriting solution to file ...\n");
        bool success_write_sol = write_matrix_to_file(output_file_sol, sol, size, 1);
        if (!success_write_sol) {
            fprintf(stderr, "Failed to save solution\n");
            return 6;
        }
        printf("\nDone\n");

        printf("Finished successfully\n");
    }

    delete[] matrix;
    delete[] rhs;
    delete[] sol;

    return 0;
}