#pragma once

#include <Eigen/Sparse>
#include <Eigen/SparseLU>
#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <vector>

class MNA {
public:
    using Matrix = Eigen::SparseMatrix<double>;
    using Triplet = Eigen::Triplet<double>;

    MNA() = default;

    void resize(int n){
        n_ = n;

        A_.resize(n,n);

        b_.resize(n);
        x_.resize(n);

        b_.setZero(n);
        x_.setZero(n);

        pattern_.clear();
        locator_.clear();
    }

    int size() const { return n_; }

    int nonZeroCount() const { return static_cast<int>(A_.nonZeros()); }

    void reservePattern(std::size_t count){
        pattern_.reserve(count);
    }

    void addPattern(int row, int col){
        pattern_.emplace_back(row, col, 0.0);
    }

    void build(){
        A_.setFromTriplets(
            pattern_.begin(),
            pattern_.end()
        );

        A_.makeCompressed();

        buildLocator();
        solver_.analyzePattern(A_);
    }

    double* ptr(int row, int col){
        auto it = locator_.find(key(row, col));

        if(it == locator_.end()){
            throw std::runtime_error("MNA::ptr(): entry not found");
        }

        return &A_.valuePtr()[it->second];
    }

    double& rhs(int row){
        return b_[row];
    }

    const Eigen::VectorXd& rhs() const{
        return b_;
    }

    void clearMatrix(){
        std::fill(
            A_.valuePtr(),
            A_.valuePtr()+A_.nonZeros(),
            0.0
        );
    }

    void clearRhs(){
        b_.setZero();
    }

    void clear(){
        clearMatrix();
        clearRhs();
    }

    bool factorize(){
        solver_.factorize(A_);
        return solver_.info() == Eigen::Success;
    }

    bool solveFactorized(const Eigen::VectorXd& rhs,
                         Eigen::VectorXd& result){
        if(rhs.size() != n_ || !rhs.allFinite()){
            return false;
        }

        result = solver_.solve(rhs);
        return solver_.info() == Eigen::Success && result.allFinite();
    }

    bool solve(){
        if(!factorize()){
            return false;
        }
        return solveFactorized(b_, x_);
    }

    const Eigen::VectorXd& solution() const{
        return x_;
    }

    Eigen::VectorXd& solution(){
        return x_;
    }

    void setSolution(const Eigen::VectorXd& x){
        // Keep the existing allocation: devices retain direct pointers into
        // x_ after bindMatrix(), so replacing its storage would invalidate
        // every nonlinear-device solution reference.
        x_ = x;
    }

    const double* solutionPtr(int row) const{
        return &x_[row];
    }

    // Pattern triplets and the lookup table are only needed while devices
    // bind their direct matrix pointers.  They can be released before the
    // iterative solve starts, where only the compressed sparse matrix remains.
    void releaseBuildMetadata(){
        std::vector<Triplet>().swap(pattern_);
        std::unordered_map<uint64_t, int>().swap(locator_);
    }

    double voltage(int node) const{
        return x_[node];
    }

    double residualInfinityNorm() const{
        if(n_ == 0){
            return 0.0;
        }

        const Eigen::VectorXd residual = A_ * x_ - b_;
        return residual.cwiseAbs().maxCoeff();
    }

    bool evaluateResidual(Eigen::VectorXd& matrixProduct,
                          Eigen::VectorXd& residual) const{
        matrixProduct = A_ * x_;
        residual = matrixProduct - b_;
        return matrixProduct.allFinite() && residual.allFinite();
    }

    bool multiplyMatrix(const Eigen::VectorXd& input,
                        Eigen::VectorXd& output) const{
        if(input.size() != n_ || !input.allFinite()){
            return false;
        }
        output = A_ * input;
        return output.allFinite();
    }

private:
    int n_ = 0;

    Matrix A_;

    Eigen::VectorXd b_;

    Eigen::VectorXd x_;

    Eigen::SparseLU<Matrix> solver_;

    std::vector<Triplet> pattern_;

    std::unordered_map<uint64_t, int> locator_;

    static uint64_t key(int row, int col){
        return
            (uint64_t(row)<<32)
            |
            uint32_t(col);
    }

    void buildLocator(){
        locator_.clear();
        locator_.reserve(static_cast<std::size_t>(A_.nonZeros()));

        const int* outer = A_.outerIndexPtr();
        const int* inner = A_.innerIndexPtr();

        const int cols = A_.cols();

        for(int col = 0; col < cols; ++col){
            for(int k = outer[col];
                k < outer[col+1];
                ++k)
            {
                int row = inner[k];
                locator_[key(row, col)] = k;
            }
        }
    }
};
