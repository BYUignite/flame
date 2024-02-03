
#include "flame.h"
#include "solver_kinsol.h"
#include "integrator_cvode.h"

#include <iostream>
#include <algorithm>        // max
#include <iomanip>
#include <fstream>
#include <sstream>

using namespace std;

////////////////////////////////////////////////////////////////////////////////

int Func_kinsol(N_Vector varsKS, N_Vector fvec, void *user_data);
int rhsf_cvode(realtype t, N_Vector varsCV, N_Vector dvarsdtCV, void *user_data);

////////////////////////////////////////////////////////////////////////////////

flame::flame(const size_t _ngrd, const double _L, double _P, shared_ptr<Cantera::Solution> csol,
             const vector<double> &_yLbc, const vector<double> &_yRbc, 
             const double _TLbc, const double _TRbc) :
    ngrd(_ngrd),
    L(_L),
    P(_P),
    yLbc(_yLbc),
    yRbc(_yRbc),
    TLbc(_TLbc),
    TRbc(_TRbc) {

    //----------

    gas = csol->thermo(); 
    kin = csol->kinetics(); 
    trn = csol->transport(); 

    nsp   = gas->nSpecies();
    nvar  = nsp + 1;            // dolh 
    nvarA = nvar*ngrd;

    y = vector<vector<double> >(ngrd, vector<double>(nsp, 0.0));
    T.resize(ngrd, 0.0);

    //---------- set grid

    setGrid(L);

    //----------

    gas->setState_TPY(TLbc, P, &yLbc[0]);
    hLbc = gas->enthalpy_mass();
    gas->setState_TPY(TRbc, P, &yRbc[0]);
    hRbc = gas->enthalpy_mass();

    strm = streams(csol, P, hLbc, hRbc, yLbc, yRbc);

    hscale = max(abs(hLbc), abs(hRbc));
    Tscale = 2500;

    //----------

    flux_y = vector<vector<double> >(ngrd+1, vector<double>(nsp, 0.0));
    flux_h.resize(ngrd+1);

    //---------- radiation object

    LdoRadiation = false;
    planckmean = new rad_planck_mean();

    Ttarget = 0.0;

}
////////////////////////////////////////////////////////////////////////////////

void flame::setGrid(double _L) {

    L = _L;
    dx = vector<double>(ngrd, L/ngrd);
    x.resize(ngrd);

    //--------- uniform grid
    // x[0] = dx[0]/2;
    // for (size_t i=1; i<ngrd; i++)
    //     x[i] = x[i-1] + (dx[i-1]+dx[i])/2;

    //--------- segmented grid
    double Lfrac = 0.2;         // first Lfrac fraction of the domain length
    double Gfrac = 0.6;         // gets this Gfrac fraction of the grid points
    int n1 = ngrd*Gfrac;
    double dx1 = L*Lfrac/n1;
    for(int i=0; i<n1; i++)
        dx[i] = dx1;

    int n2 = ngrd-n1;
    double dx2 = L*(1.0-Lfrac)/n2;
    for(int i=n1; i<ngrd; i++)
        dx[i] = dx2;

    x[0] = dx[0]/2;
    for (size_t i=1; i<ngrd; i++)
        x[i] = x[i-1] + (dx[i-1]+dx[i])/2;
}

////////////////////////////////////////////////////////////////////////////////

void flame::writeFile(string fname) {

    //-------------- compute auxiliary quantities

    vector<double> mixf(ngrd, 0.0);
    for(int i=0; i<ngrd; i++)
        mixf[i] = strm.getMixtureFraction(&y[i][0]);
    double mixfLbc = strm.getMixtureFraction(&yLbc[0]);
    double mixfRbc = strm.getMixtureFraction(&yRbc[0]);

    vector<double> rho(ngrd);
    vector<double> h(ngrd);
    for(int i=0; i<ngrd; i++) {
        gas->setState_TPY(T[i], P, &y[i][0]);
        rho[i] = gas->density();
        h[i] = gas->enthalpy_mass();
    }
    gas->setState_TPY(TLbc, P, &yLbc[0]);
    double rhoLbc = gas->density();
    gas->setState_TPY(TRbc, P, &yRbc[0]);
    double rhoRbc = gas->density();

    //-------------- 

    ofstream ofile(fname.c_str());
    if(!ofile) {
        cerr << "\n\n***************** ERROR OPENING FILE " << fname << endl << endl;
        exit(0);
    }

    ofile << "# L (m)  = " << L << endl;
    ofile << "# P (Pa) = " << P << endl;

    ofile << "#";
    int j=1;
    ofile << setw(15) << "00" << j++ << "_x";
    ofile << setw(13) << "00" << j++ << "_mixf";
    ofile << setw(16) << "00" << j++ << "_T";
    ofile << setw(16) << "00" << j++ << "_h";
    ofile << setw(10) << "00" << j++ << "_density";
    for(int k=0; k<nsp; k++) {
        stringstream ss; ss << setfill('0') << setw(3) << j++ << "_" << gas->speciesName(k);
        ofile << setw(19) << ss.str();
    }

    ofile << scientific;
    ofile << setprecision(10);

    ofile << endl;
    ofile << setw(19) << 0;
    ofile << setw(19) << mixfLbc;
    ofile << setw(19) << TLbc;
    ofile << setw(19) << hLbc;
    ofile << setw(19) << rhoLbc;
    for(int k=0; k<nsp; k++)
        ofile << setw(19) << yLbc[k];

    for(int i=0; i<ngrd; i++) {
        ofile << endl;
        ofile << setw(19) << x[i];
        ofile << setw(19) << mixf[i];
        ofile << setw(19) << T[i];
        ofile << setw(19) << h[i];
        ofile << setw(19) << rho[i];
        for(int k=0; k<nsp; k++)
            ofile << setw(19) << y[i][k];
    }

    ofile << endl;
    ofile << setw(19) << L;
    ofile << setw(19) << mixfRbc;
    ofile << setw(19) << TRbc;
    ofile << setw(19) << hRbc;
    ofile << setw(19) << rhoRbc;
    for(int k=0; k<nsp; k++)
        ofile << setw(19) << yRbc[k];

    ofile.close();
}

////////////////////////////////////////////////////////////////////////////////

void flame::storeState() {

    Pstore = P;
    ystore = y;
    Tstore = T;
}

////////////////////////////////////////////////////////////////////////////////

void flame::setIC(std::string icType, string fname) {


    //-------------------

    if (icType == "linear") {
        gas->setState_TPY(TLbc, P, &yLbc[0]);
        double hLbc = gas->enthalpy_mass();
        gas->setState_TPY(TRbc, P, &yRbc[0]);
        double hRbc = gas->enthalpy_mass();
        for(int i=0; i<ngrd; i++) {
            for(int k=0; k<nsp; k++)
                y[i][k] = yLbc[k] + x[i]/L*(yRbc[k]-yLbc[k]);
            double h = hLbc + x[i]/L*(hRbc-hLbc);
            gas->setMassFractions(&y[i][0]);
            gas->setState_HP(h,P);
            T[i] = gas->temperature();
        }
    }

    //-------------------

    else if (icType == "equilibrium") {
        setIC("linear");
        for(int i=0; i<ngrd; i++) {
            gas->setState_TPY(T[i], P, &y[i][0]);
            gas->equilibrate("HP");
            gas->getMassFractions(&y[i][0]);
            T[i] = gas->temperature();
        }
    }

    else if (icType == "stored") {
        P = Pstore;
        y = ystore;
        T = Tstore;
    }

    //-------------------

    // else if (icType == "file") {{{{
    //     ifstream ifile(fname.c_str());
    //     if(!ifile) {
    //         cerr << "\n\n***************** ERROR OPENING FILE " << fname << endl << endl;
    //         exit(0);
    //     }
    //     string s1;
    //     getline(ifile, s1);       // get header lines
    //     getline(ifile, s1);
    //     getline(ifile, s1);

    //     vector<double> XX;
    //     vector<double> HH;
    //     vector<vector<double> > YY;
    //     double d;
    //     while(!ifile.eof()) {
    //         ifile >> d; XX.push_back(d);   // x
    //         ifile >> d;                    // T
    //         ifile >> d; HH.push_back(d);   // h
    //         YY.push_back(vector<double>());
    //         for(int k=0; k<nsp; k++) {
    //             ifile >> d; YY.back().push_back(d);    
    //         }
    //     }

    // }}}}

    //-------------------

    storeState();

}

////////////////////////////////////////////////////////////////////////////////
// assuming unity Le for all species

void flame::setFluxesUnity() {

    //---------- cell center density and diffusivity

    vector<double> D(ngrd);
    vector<double> density(ngrd);
    vector<double> h(ngrd);
    for(int i=0; i<ngrd; i++) {
        gas->setMassFractions_NoNorm(&y[i][0]);
        gas->setState_TP(T[i], P);
        density[i] = gas->density();
        D[i] = trn->thermalConductivity()/(density[i]*gas->cp_mass());
        h[i] = gas->enthalpy_mass();
    }

    //---------- interpolate to face center density and diffusivity


    vector<double> D_f(ngrd+1);
    vector<double> density_f(ngrd+1);

    gas->setState_TPY(TLbc, P, &yLbc[0]);
    density_f[0] = gas->density();
    D_f[0] = trn->thermalConductivity()/(density_f[0]*gas->cp_mass());

    gas->setState_TPY(TRbc, P, &yRbc[0]);
    density_f.back() = gas->density();
    D_f.back() = trn->thermalConductivity()/(density_f.back()*gas->cp_mass());

    for(int i=1; i<ngrd; i++) {
        double f1 = dx[i-1]/(dx[i-1]+dx[i]);
        double f0 = 1.0-f1;
        density_f[i] = density[i-1]*f0 + density[i]*f1;
        D_f[i] = D[i-1]*f0 + D[i]*f1;
    }

    //---------- fluxes y

    for(int k=0; k<nsp; k++) {
        if(LisPremixed) {
            flux_y[0][k]    = mflux * yLbc[k];
            flux_y[ngrd][k] = mflux * y[ngrd-1][k];
        }
        else {
            flux_y[0][k]    = -density_f[0]    *D_f[0]    *(y[0][k]-yLbc[k])     *2/dx[0];
            flux_y[ngrd][k] = -density_f.back()*D_f.back()*(yRbc[k]-y[ngrd-1][k])*2/dx.back();
        }
        for(int i=1; i<ngrd; i++) {
            flux_y[i][k] = -density_f[i]*D_f[i]*(y[i][k]-y[i-1][k])*2/(dx[i-1]+dx[i]);
            if(LisPremixed) flux_y[i][k] += mflux*y[i-1][k];
        }
    }

    //---------- fluxes h

    for(int i=1; i<ngrd; i++) {
        flux_h[i] = -density_f[i]*D_f[i]*(h[i]-h[i-1])*2/(dx[i-1]+dx[i]);
        if(LisPremixed) flux_h[i] += mflux*h[i-1];
    }
    if(!LisPremixed) {
        flux_h.back() = -density_f.back()*D_f.back()*(hRbc-h.back())*2/dx.back(); 
        flux_h[0]     = -density_f[0]*D_f[0]*(h[0]-hLbc)*2/dx[0];
    }
    else {
        flux_h.back() = mflux*h[ngrd-1];

        flux_h[0] = 0.0;
        vector<double> hsp(nsp);                     // J/kg species i
        gas->getEnthalpy_RT(&hsp[0]);                // (hhat/RT) where hhat = J/kmol
        for(size_t k=0; k<nsp; k++){                  // --> hsp = J/kg species i
            hsp[k] *= TLbc*Cantera::GasConstant/gas->molecularWeight(k);
            flux_h[0] += hsp[k]*flux_y[0][k];
        }
        gas->setState_TPY(TLbc, P, &yLbc[0]);
        flux_h[0] -= trn->thermalConductivity() * (T[1]-TLbc)/(dx[0]*0.5);
    }
}

//////////////////////////////////////////////////////////////////////////////////
////  Non-unity Le for all species

void flame::setFluxes() {

    //---------- cell center density and diffusivity
    vector<vector<double>> D(ngrd, vector<double> (nsp,0.0));
    vector<double> density(ngrd);
    vector<double> M(ngrd);
    vector<double> tcond(ngrd);
    for(int i=0; i<ngrd; i++) {
        gas->setMassFractions_NoNorm(&y[i][0]);
        gas->setState_TP(T[i], P);
        density[i] = gas->density();
        M[i] = gas->meanMolecularWeight();
        trn->getMixDiffCoeffs(&D[i][0]);
        tcond[i] = trn->thermalConductivity();
    }

    //---------- interpolate to face center density and diffusivity

    vector<vector<double>> D_f(ngrd+1, vector<double> (nsp,0.0));
    vector<vector<double>> y_f(ngrd+1, vector<double> (nsp,0.0));
    vector<double> density_f(ngrd+1);
    vector<double> M_f(ngrd+1);
    vector<double> tcond_f(ngrd+1);
    vector<double> T_f(ngrd+1);


    gas->setState_TPY(TLbc, P, &yLbc[0]);
    density_f[0] = gas->density();
    M_f[0] = gas->meanMolecularWeight();
    trn->getMixDiffCoeffs(&D_f[0][0]);
    tcond_f[0] = trn->thermalConductivity();
    T_f[0] = TLbc;
    y_f[0] = yLbc;

    gas->setState_TPY(TRbc, P, &yRbc[0]);
    density_f.back() = gas->density();
    M_f.back() = gas->meanMolecularWeight();
    trn->getMixDiffCoeffs(&(D_f.back()[0]));
    tcond_f.back() = trn->thermalConductivity();
    T_f.back() = TRbc;
    y_f.back() = yRbc;

    for (int i=1; i<ngrd; i++) {
        double f1 = dx[i-1]/(dx[i-1]+dx[i]);
        double f0 = 1.0-f1;
        density_f[i] = density[i-1]*f0 + density[i] * f1;
        T_f[i]       = T[i-1]      *f0 + T[i]       * f1;
        tcond_f[i]   = tcond[i-1]  *f0 + tcond[i]   * f1;
        M_f[i]       = M[i-1]      *f0 + M[i]       * f1;
        for(int k=0; k<nsp; k++) {
            D_f[i][k] = D[i-1][k]*f0 + D[i][k]*f1;
            y_f[i][k] = y[i-1][k]*f0 + y[i][k]*f1;
        }
    }

    //---------- fluxes y

    double jstar;             // correction flux so that all fluxes sum to zero. This is equal to using a correction velocity
                              // j_i_corrected = j_i - Yi*jstar; jstar = sum(j_i).

    for (int i=1; i<ngrd; i++) {
        jstar = 0.0;
        for(int k=0; k<nsp; k++) {
            flux_y[i][k] = -density_f[i]*D_f[i][k]*(y[i][k]-y[i-1][k])*2/(dx[i-1]+dx[i])\
                           -density_f[i]*D_f[i][k]*y_f[i][k]*(M[i]-M[i-1])*2/(dx[i-1]+dx[i])/M_f[i];
            jstar += flux_y[i][k];
        }
        for(int k=0; k<nsp; k++) {
            flux_y[i][k] -= y_f[i][k]*jstar;
        }
    }
    //--------- Boundary faces
    //Left boundary
    jstar = 0.0;
    for(int k=0; k<nsp; k++) {
        flux_y[0][k]    = -density_f[0]    *D_f[0][k]    *(y[0][k]-yLbc[k])     *2/dx[0]\
                          -density_f[0]    *D_f[0][k]    *y_f[0][k]    *(M[0]-M_f[0])         *2/dx[0];
        jstar += flux_y[0][k];
    }	
    for(int k=0; k<nsp; k++) {
        flux_y[0][k] -= y_f[0][k]*jstar;
    }
    //Right boundary
    jstar = 0.0;
    for(int k=0; k<nsp; k++) {
        flux_y[ngrd][k] = -density_f.back()*D_f.back()[k]*(yRbc[k]-y[ngrd-1][k])*2/dx.back()\
                          -density_f.back()*D_f.back()[k]*y_f.back()[k]*(M_f.back()-M[ngrd-1])*2/dx.back();
        jstar += flux_y[ngrd][k];
    }
    for(int k=0; k<nsp; k++) {
        flux_y[ngrd][k] -= y_f[ngrd][k]*jstar;
    }

    //---------- fluxes h

    flux_h[0]     = -tcond_f[0]*(T[0]-TLbc)*2/dx[0];
    flux_h.back() = -tcond_f.back()*(TRbc-T.back())*2/dx.back(); 
    for(int i=1; i<ngrd; i++)
        flux_h[i] = -tcond_f[i]*(T[i]-T[i-1])*2/(dx[i-1]+dx[i]);

    vector<double> hsp(nsp);

    gas->setState_TPY(TLbc, P, &yLbc[0]);
    gas->getEnthalpy_RT(&hsp[0]);
    for(size_t k=0; k<nsp; k++)
        flux_h[0] += flux_y[0][k]*hsp[k]*TLbc*Cantera::GasConstant/gas->molecularWeight(k);

    gas->setState_TPY(TRbc, P, &yRbc[0]);
    gas->getEnthalpy_RT(&hsp[0]);
    for(size_t k=0; k<nsp; k++)
        flux_h.back() += flux_y.back()[k]*hsp[k]*TRbc*Cantera::GasConstant/gas->molecularWeight(k);

    for(size_t i=1; i<ngrd; i++) {
        gas->setState_TP(T_f[i], P);        // hsp depends on T but not on y
        gas->getEnthalpy_RT(&hsp[0]);
        for(size_t k=0; k<nsp; k++)
            flux_h[i] += flux_y[i][k]*hsp[k]*T_f[i]*Cantera::GasConstant/gas->molecularWeight(k);
    }
}

////////////////////////////////////////////////////////////////////////////////
// assumes y, T are initialized

void flame::solveSS() {

    //---------- transfer variables into single array

    vector<double> vars(nvarA);

    for(size_t i=0; i<ngrd; i++) {
        for(size_t k=0; k<nsp; k++)
            vars[Ia(i,k)] = y[i][k];
        vars[Ia(i,nvar-1)] = T[i]/Tscale;      // dolh comment to remove h
    }
    
    //-------------- set vars0, F0 for homotopy

    vars0 = vars;
    F0.resize(nvarA);
    Func(&vars0[0], &F0[0]);

    //---------- setup solver

    vector<double> scales_v(nvarA, 1.0);
    vector<double> scales_f(nvarA, 1.0);
    vector<double> constraints(nvarA, 0.0);

    for(int i=0; i<nvarA; i++) {
        scales_v[i] = vars[i] < 1E-3 ? 1E-3 : vars[i];
        scales_f[i] = vars[i] < 1E-3 ? 1E-3 : vars[i];
    }


    double ftol = 5E-6; //5E-4;   // default: 5E-6
    double stol = 2E-11;  // default: 2E-11
    int mu = nvar*2-1;
    int ml = nvar*2-1;
    solver_kinsol s_kin(this, nvarA, scales_v, scales_f, constraints, mu, ml, ftol, stol);

    //---------- solve

    // s = 0.0;
    // s_kin.solve(Func_kinsol, vars);
    // s = 0.5;
    // s_kin.solve(Func_kinsol, vars);

    for(s=0.0; s<=1.0; s+=0.01) {
        cout << endl << "s = " << s; cout.flush();
        s_kin.solve(Func_kinsol, vars);
    }

    //---------- transfer variables back

    for(size_t i=0; i<ngrd; i++) {
        for(size_t k=0; k<nsp; k++)
            y[i][k] = vars[Ia(i,k)];
        T[i] = vars[Ia(i,nvar-1)]*Tscale;      // dolh comment to remove h
    }
}

////////////////////////////////////////////////////////////////////////////////

int flame::Func(const double *vars, double *F) {

    //------------ transfer variables

    for(size_t i=0; i<ngrd; i++) {
        for(size_t k=0; k<nsp; k++)
            y[i][k] = vars[Ia(i,k)];
        T[i] = vars[Ia(i,nvar-1)]*Tscale;      // dolh comment to remove h
    }

    //------------ set function values

    //setFluxes();
    setFluxesUnity();

    vector<double> Q(ngrd);
    if(LdoRadiation) setQrad(Q);

    vector<double> rr(nsp);

    for(size_t i=0; i<ngrd; i++) {
        gas->setMassFractions_NoNorm(&y[i][0]);
        gas->setState_TP(T[i], P);
        kin->getNetProductionRates(&rr[0]);
        for(size_t k=0; k<nsp; k++)
            F[Ia(i,k)]  = flux_y[i+1][k] - flux_y[i][k]
                         - rr[k]*gas->molecularWeight(k)*dx[i];
        F[Ia(i,nvar-1)] = (flux_h[i+1] - flux_h[i])/hscale; // dolh comment to remove h

        if(LdoRadiation) F[Ia(i,nvar-1)] -= Q[i]/hscale;
    }

    //------------ augment for homotopy

    for(size_t i=0; i<nvarA; i++)
        F[i] = F[i] + (s-1.0)*F0[i];

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Kinsol interface; Kinsol calls this function, which then calls user_data's Func 

int Func_kinsol(N_Vector varsKS, N_Vector fvec, void *user_data) {

    flame *flm = static_cast<flame *>(user_data);

    double *vars = N_VGetArrayPointer(varsKS);
    double *F = N_VGetArrayPointer(fvec);

    int rv = flm->Func(vars, F);

    return rv;
}







////////////////////////////////////////////////////////////////////////////////

void flame::setQrad(vector<double> &Q) {

    vector<double> kabs, awts;
    double fvsoot = 0.0;
    double xH2O, xCO2, xCO, xCH4;
    int isp;

    for(int i=0; i<ngrd; i++) {
        isp = gas->speciesIndex("H2O"); 
        xH2O = y[i][isp]/gas->molecularWeight(isp)*gas->meanMolecularWeight();
        isp = gas->speciesIndex("CO2"); 
        xCO2 = y[i][isp]/gas->molecularWeight(isp)*gas->meanMolecularWeight();
        isp = gas->speciesIndex("CO"); 
        xCO = y[i][isp]/gas->molecularWeight(isp)*gas->meanMolecularWeight();
        isp = gas->speciesIndex("CH4"); 
        xCH4 = y[i][isp]/gas->molecularWeight(isp)*gas->meanMolecularWeight();
        planckmean->get_k_a(kabs, awts, T[i], P, fvsoot, xH2O, xCO2, xCO, xCH4);

        Q[i] = -4.0*rad::sigma*kabs[0]*(pow(T[i],4.0) - pow(TLbc,4.0));
    }
}

////////////////////////////////////////////////////////////////////////////////
// assumes y, T are initialized
// Two modes: write on every time step of size dt, or write on temperature steps of size dT
// Default is in time --> Tmin, Tmax are zero --> dT = 0
// Lwrite = true (default) --> write in time;

void flame::solveUnsteady(double nTauRun, int nsteps, bool LwriteTime, double Tmin, double Tmax) {

    //---------- transfer variables into single array

    vector<double> vars(nvarA);

    for(size_t i=0; i<ngrd; i++) {
        for(size_t k=0; k<nsp; k++)
            vars[Ia(i,k)] = y[i][k];
        vars[Ia(i,nvar-1)] = T[i]/Tscale;      // dolh comment to remove h
    }
    
    //---------- setup solver

    double rtol = 1E-4;                        // 1E-4 --> error at 0.01%, cvode documentation recomends < 1E-3
    vector<double> atol(nvarA, 1.0);           // noise level of the given variable

    for(int i=0; i<nvarA; i++)
        atol[i] = 1E-12;
        //atol[i] = vars[i] < 1E-3 ? 1E-3 : vars[i];


    int mu = nvar*2-1;
    int ml = nvar*2-1;
    integrator_cvode integ(rhsf_cvode, this, nvarA, rtol, atol, mu, ml, vars);

    //---------- solve

    double D = 0.00035;     // avg thermal diffusivity
    double t = 0.0;
    double tend = nTauRun*L*L/D;
    double dt = tend/nsteps;
    dT = Tmax==Tmin ? 0.0 : (Tmax-(Tmin+0.1))/nsteps;
    Ttarget = Tmax - dT;
    isave = 1;

    for(int istep=1; istep<=nsteps; istep++, t+=dt) {

        integ.integrate(vars, dt);

        if(LwriteTime && dT <= 0.0) {           // write in time; (write in Temp is in rhsf)
            stringstream ss; ss << "L_" << L << "U_" << setfill('0') << setw(3) << isave++ << ".dat";
            string fname = ss.str();
            writeFile(fname);
        }
    }

    //---------- transfer variables back

    for(size_t i=0; i<ngrd; i++) {
        for(size_t k=0; k<nsp; k++)
            y[i][k] = vars[Ia(i,k)];
        T[i] = vars[Ia(i,nvar-1)]*Tscale;      // dolh comment to remove h
    }

    Ttarget = 0.0;
}

////////////////////////////////////////////////////////////////////////////////

int flame::rhsf(const double *vars, double *dvarsdt) {

    //------------ transfer variables

    for(size_t i=0; i<ngrd; i++) {
        for(size_t k=0; k<nsp; k++)
            y[i][k] = vars[Ia(i,k)];
        T[i] = vars[Ia(i,nvar-1)]*Tscale;      // dolh comment to remove h
    }
//------------ set rates dvarsdt (dvars/dt)

    //setFluxes();
    setFluxesUnity();

    vector<double> rr(nsp);
    vector<double> Q(ngrd);
    if(LdoRadiation) setQrad(Q);

    for(size_t i=0; i<ngrd; i++) {
        gas->setMassFractions_NoNorm(&y[i][0]);
        gas->setState_TP(T[i], P);
        kin->getNetProductionRates(&rr[0]);          // kmol/m3*s
        double rho = gas->density(); 
        for(size_t k=0; k<nsp; k++)
            dvarsdt[Ia(i,k)]  = -(flux_y[i+1][k] - flux_y[i][k])/(rho*dx[i]) + 
                                rr[k]*gas->molecularWeight(k)/rho;

        double cp  = gas->cp_mass();
        vector<double> hsp(nsp);                     // J/kg species i
        gas->getEnthalpy_RT(&hsp[0]);                // (hhat/RT) where hhat = J/kmol
        for(size_t k=0; k<nsp; k++)                  // --> hsp = J/kg species i
            hsp[k] *= T[i]*Cantera::GasConstant/gas->molecularWeight(k);
        double sum_hkdykdt = 0.0;
        for(size_t k=0; k<nsp; k++)
            sum_hkdykdt += hsp[k]*dvarsdt[Ia(i,k)];
        dvarsdt[Ia(i,nvar-1)]  =  -(flux_h[i+1] - flux_h[i])/(rho*cp*dx[i]) - sum_hkdykdt/cp;
        if(LdoRadiation) dvarsdt[Ia(i,nvar-1)] += Q[i]/(rho*cp);
        dvarsdt[Ia(i,nvar-1)] /= Tscale;
    }

    //-------------

    double TmaxLocal = *max_element(T.begin(), T.end());
    if(TmaxLocal <= Ttarget) {
        cout << endl << isave << "  " << TmaxLocal << "  " << Ttarget << "  ";
        stringstream ss; ss << "L_" << L << "U_" << setfill('0') << setw(3) << isave++ << ".dat";
        string fname = ss.str();
        writeFile(fname);
        Ttarget -= dT;
    }

    //-------------

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// CVODE interface; CVODE calls this function, which then calls user_data's rhsf 

int rhsf_cvode(realtype t, N_Vector varsCV, N_Vector dvarsdtCV, void *user_data) {

    flame *flm = static_cast<flame *>(user_data);

    double *vars  = N_VGetArrayPointer(varsCV);
    double *dvarsdt = N_VGetArrayPointer(dvarsdtCV);

    int rv = flm->rhsf(vars, dvarsdt);

    return rv;
}
