def initialize_mutations():
    mutations = []
    mutation_insert_r = "inserting one resistance element"
    mutations.append(mutation_insert_r)
    mutation_insert_c = "inserting one capacitor element"
    mutations.append(mutation_insert_c)
    mutation_insert_l = "inserting one inductor element"
    mutations.append(mutation_insert_l)
    mutation_insert_i = "inserting one current source element"
    mutations.append(mutation_insert_i)
    mutation_insert_v = "inserting one voltage source element"
    mutations.append(mutation_insert_v)
    mutation_insert_d = "inserting one diode element"
    mutations.append(mutation_insert_d)
    mutation_insert_q1 = "inserting one NPN transistor element"
    mutations.append(mutation_insert_q1)
    mutation_insert_q2 = "inserting one PNP transistor element"
    mutations.append(mutation_insert_q2)
    mutation_insert_j1 = "inserting one NJF transistor element"
    mutations.append(mutation_insert_j1)
    mutation_insert_j2 = "inserting one PJF transistor element"
    mutations.append(mutation_insert_j2)
    mutation_insert_m1 = "inserting one NMOS transistor element"
    mutations.append(mutation_insert_m1)
    mutation_insert_m2 = "inserting one PMOS transistor element"
    mutations.append(mutation_insert_m2)
    mutation_insert_m3 = "inserting one VDMOS transistor element"
    mutations.append(mutation_insert_m3)
    mutation_insert_s = "inserting one voltage controlled switch element"
    mutations.append(mutation_insert_s)
    mutation_insert_w = "inserting one current controlled switch element"
    mutations.append(mutation_insert_w)
    mutation_insert_sub_circuit = "inserting one subcircuit"
    mutations.append(mutation_insert_sub_circuit)
    mutation_insert_func = "inserting one function block"
    mutations.append(mutation_insert_func)
    mutation_insert_param = "inserting one param line"
    mutations.append(mutation_insert_param)
    mutation_insert_if = "inserting one IF control block"
    mutations.append(mutation_insert_if)
    mutation_change_to_dc = "changing the analysis type to DC analysis"
    mutations.append(mutation_change_to_dc)
    mutation_change_to_ac = "changing the analysis type to AC Small-Signal analysis"
    mutations.append(mutation_change_to_ac)
    mutation_change_to_tran = "changing the analysis type to Transient analysis"
    mutations.append(mutation_change_to_tran)
    mutation_change_to_pz = "changing the analysis type to Pole-Zero analysis"
    mutations.append(mutation_change_to_pz)
    mutation_change_to_noise = "changing the analysis type to Noise analysis"
    mutations.append(mutation_change_to_noise)
    mutation_delete = "deleting one element"
    mutations.append(mutation_delete)
    return mutations


def initialize_forms():
    forms = []
    form_of_r = "The general form of resistance element is '''RXXX n+ n- <r=>value <ac=val> <m=val> <scale=val> <temp=val> <dtemp=val> <tc1=val> <tc2=val> <noisy=0|1>'''."
    forms.append(form_of_r)
    form_of_c = "The general form of capacitor element is '''CXXX n+ n- <value> <mname> <m=val> <scale=val> <temp=val> <dtemp=val> <tc1=val> <tc2=val> <ic=init_condition>'''."
    forms.append(form_of_c)
    form_of_l = "The general form of inductor element is '''LYYY n+ n- <value> <mname> <nt=val> <m=val> <scale=val> <temp=val> <dtemp=val> <tc1=val> <tc2=val> <ic=init_condition>'''."
    forms.append(form_of_l)
    form_of_i = "The general form of current source element is '''IYYY N+ N- <<DC> DC/TRAN VALUE> <AC <ACMAG <ACPHASE>>> <DISTOF1 <F1MAG <F1PHASE>>> <DISTOF2 <F2MAG <F2PHASE>>>'''."
    forms.append(form_of_i)
    form_of_v = "The general form of voltage source element is '''VXXX N+ N- <<DC> DC/TRAN VALUE> <AC <ACMAG <ACPHASE>>> <DISTOF1 <F1MAG <F1PHASE>>> <DISTOF2 <F2MAG <F2PHASE>>>'''."
    forms.append(form_of_v)
    form_of_d = "The general form of diode element is '''DXXX n+ n- mname <area=val> <m=val> <pj=val> <off> <ic=vd> <temp=val> <dtemp=val> <lm=val> <wm=val> <lp=val> <wp=val>'''. The general form of diode model is '''.model D type (pname1=pval1 pname2=pval2 ... )'''."
    forms.append(form_of_d)
    form_of_q1 = "The general form of NPN transistor element is '''QXXX nc nb ne <ns> <tj> mname <area=val> <areac=val> <areab=val> <m=val> <off> <ic=vbe,vce> <temp=val> <dtemp=val>'''. The general form of NPN model is '''.model mname NPN (pname1=pval1 pname2=pval2 ... )'''."
    forms.append(form_of_q1)
    form_of_q2 = "The general form of PNP transistor element is '''QXXX nc nb ne <ns> <tj> mname <area=val> <areac=val> <areab=val> <m=val> <off> <ic=vbe,vce> <temp=val> <dtemp=val>'''. The general form of PNP model is '''.model mname PNP (pname1=pval1 pname2=pval2 ... )'''."
    forms.append(form_of_q2)
    form_of_j1 = "The general form of NJF element is '''JXXX nd ng ns mname <area> <off> <ic=vds, vgs> <temp=t>'''. The general form of NJF model is '''.model mname NJF (pname1=pval1 pname2=pval2 ... )'''."
    forms.append(form_of_j1)
    form_of_j2 = "The general form of PJF element is '''JXXX nd ng ns mname <area> <off> <ic=vds, vgs> <temp=t>'''. The general form of PJF model is '''.model mname PJF (pname1=pval1 pname2=pval2 ... )'''."
    forms.append(form_of_j2)
    form_of_m1 = "The general form of NMOS element is '''MXXX nd ng ns nb mname <m=val> <l=val> <w=val> <ad=val> <as=val> <pd=val> <ps=val> <nrd=val> <nrs=val> <off> <ic=vds, vgs, vbs> <temp=t>'''. The general form of NMOS model is '''.model mname NMOS (pname1=pval1 pname2=pval2 ... )'''."
    forms.append(form_of_m1)
    form_of_m2 = "The general form of PMOS element is '''MXXX nd ng ns nb mname <m=val> <l=val> <w=val> <ad=val> <as=val> <pd=val> <ps=val> <nrd=val> <nrs=val> <off> <ic=vds, vgs, vbs> <temp=t>'''. The general form of PMOS model is '''.model mname PMOS (pname1=pval1 pname2=pval2 ... )'''."
    forms.append(form_of_m2)
    form_of_m3 = "The general form of VDMOS element is '''MXXX nd ng ns mname <m=val> <temp=t> <dtemp=t>'''. The general form of VDMOS model is '''.model mname VDMOS (pname1=pval1 pname2=pval2 ... )'''."
    forms.append(form_of_m3)
    form_of_s = "The general form of voltage controlled switch element is '''SXXX N+ N- NC+ NC- MODEL <ON> <OFF>'''. The general form of voltage controlled switch model is '''.model mname SW (pname1=pval1 pname2=pval2 ... )'''."
    forms.append(form_of_s)
    form_of_w = "The general form of current controlled switch element is '''WYYY N+ N- VNAM MODEL <ON> <OFF>'''. The general form of current controlled switch model is '''.model mname CSW (pname1=pval1 pname2=pval2 ... )'''."
    forms.append(form_of_w)
    form_of_x = "The general form of subcircuit is '''XYYYYYYY N1 <N2 N3 ... > SUBNAM'''. The general form of current controlled switch model is '''.SUBCKT subnam N1 <N2 N3 ... > \n ... \n .ENDS <SUBNAM>'''."
    forms.append(form_of_x)
    form_of_func = "The general form of function block is '''.func <ident> { <expr> }''' or '''.func <ident> = { <expr> }'''."
    forms.append(form_of_func)
    form_of_parm = "The general form of parm line is '''.param <ident> = <expr> <ident> = <expr> ...'''."
    forms.append(form_of_parm)
    form_of_if = "The general form of if control block is '''.if ( boolean expression ) \n ... \n .elseif ( boolean expression ) \n ... \n .else \n ... \n .endif'''."
    forms.append(form_of_if)
    form_of_dc = "The general form of DC analysis is '''.dc srcnam vstart vstop vincr [src2 start2 stop2 incr2]'''."
    forms.append(form_of_dc)
    form_of_ac = "The general forms of AC Small-Signal analysis are '''.ac dec nd fstart fstop''' or '''.ac oct no fstart fstop''' or '''.ac lin np fstart fstop'''."
    forms.append(form_of_ac)
    form_of_tran = "The general form of Transient analysis is '''.tran tstep tstop <tstart <tmax>> <uic>'''."
    forms.append(form_of_tran)
    form_of_pz = "The general forms of Pole-Zero analysis are '''.pz node1 node2 node3 node4 cur pol''' or '''.pz node1 node2 node3 node4 cur zer''' or '''.pz node1 node2 node3 node4 cur pz''' or '''.pz node1 node2 node3 node4 vol pol''' or '''.pz node1 node2 node3 node4 vol zer'''  or '''.pz node1 node2 node3 node4 vol pz'''."
    forms.append(form_of_pz)
    form_of_noise = "The general form of Noise analysis is '''.noise v ( output <, ref >) src (dec | lin | oct ) pts fstart fstop < pts_per_summary >'''."
    forms.append(form_of_noise)
    return forms


# The PTA simulator implements a deliberately restricted SPICE subset.  Keep
# this catalogue separate so the original ngspice fuzzing entry point retains
# its complete mutation space.
PTA_MUTATION_INDICES = (0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 15, 24)

# Syntax guidance for the twelve retained generic operators.  These forms are
# intentionally narrower than initialize_forms(): that catalogue describes
# ngspice, while PTA accepts only a compatibility subset.
PTA_BASE_FORMS = (
    "Use: Rname n_plus n_minus positive_value. A resistor may be used to add a finite DC return path or a small physical asymmetry.",
    "Use: Cname n_plus n_minus positive_value [IC=value]. In .op a capacitor is open-circuit, so both terminals still need independent finite DC paths.",
    "Use: Lname n_plus n_minus positive_value [IC=value]. In .op an inductor is a DC short with a branch unknown; do not create an ideal-voltage-source or inductor loop.",
    "Use: Iname n_plus n_minus [DC] finite_value. Only an independent constant DC current source is allowed.",
    "Use: Vname n_plus n_minus [DC] finite_value. Only an independent constant DC voltage source is allowed; avoid conflicting parallel sources and source loops.",
    "Use: Dname anode cathode model [AREA=positive_value] [OFF] [IC=value], with a .model model D card. Prefer common PTA/ngspice model parameters such as IS, N, and RS.",
    "Use: Qname collector base emitter model [AREA=positive_value] [OFF] [IC=vbe,vce], with a .model model NPN card. Prefer common PTA/ngspice parameters such as IS, BF, BR, NF, NR, VA, and IKF.",
    "Use: Qname collector base emitter model [AREA=positive_value] [OFF] [IC=vbe,vce], with a .model model PNP card. Prefer common PTA/ngspice parameters such as IS, BF, BR, NF, NR, VA, and IKF.",
    "Use: Mname drain gate source bulk model [L=positive_value] [W=positive_value] [AD=value] [AS=value] [PD=value] [PS=value] [NRD=value] [NRS=value] [M=positive_value] [OFF] [IC=vds,vgs,vbs], with a LEVEL=1 or LEVEL=3 .model model NMOS card.",
    "Use: Mname drain gate source bulk model [L=positive_value] [W=positive_value] [AD=value] [AS=value] [PD=value] [PS=value] [NRD=value] [NRS=value] [M=positive_value] [OFF] [IC=vds,vgs,vbs], with a LEVEL=1 or LEVEL=3 .model model PMOS card.",
    "Define .subckt name terminal... followed only by supported primitive devices and .ends name; instantiate it as Xname node... name. Do not nest subcircuits or place control directives inside one.",
    "Delete exactly one element or removable model/subcircuit definition. Repair references and preserve a finite DC path for every remaining node; do not make the MNA system singular.",
)

# Directed operators for finding DC operating-point decks that are physically
# valid but difficult for Newton-family solvers.  The guidance intentionally
# avoids solver options: the same generated deck must be replayable under each
# ablation configuration without giving one method an artificial advantage.
PTA_DIRECTED_MUTATION_SPECS = (
    (
        "creating a cross-coupled CMOS positive-feedback loop",
        "Create two supported Level-1 or Level-3 CMOS stages whose outputs drive each other's inputs. Keep a finite DC bias path on every node and use a small physical asymmetry so the circuit is not exactly singular.",
    ),
    (
        "creating a regenerative cross-coupled BJT pair",
        "Use supported NPN or PNP models to create regenerative collector/base coupling. Include finite emitter, collector, and bias paths; do not leave any base or collector floating.",
    ),
    (
        "closing a nonlinear stage into positive feedback",
        "Reconnect an existing MOS, BJT, or diode stage so that an output influences an earlier control node with positive feedback. Preserve all required DC return paths and avoid ideal-source loops.",
    ),
    (
        "duplicating a nearly symmetric nonlinear branch",
        "Duplicate a transistor or diode branch around a shared bias network, then introduce one small parameter or resistance mismatch. The result should be nearly symmetric but have a deterministic physical DC solution branch.",
    ),
    (
        "stacking nonlinear devices into a stiff DC path",
        "Create a series stack of supported diodes, BJTs, or MOSFETs with intermediate nodes. Give every intermediate node a finite DC path and keep device polarities and model names valid.",
    ),
    (
        "increasing transistor gain or transconductance",
        "Increase supported gain-setting parameters such as MOS W/L or KP, or BJT BF, within finite positive ranges. Preserve the topology and avoid unsupported model parameters.",
    ),
    (
        "moving semiconductor parameters toward a steep nonlinear region",
        "Mutate supported parameters such as diode IS/N/RS, MOS VTO/KP/LAMBDA, or BJT IS/BF/VA/IKF so the operating point lies near strong nonlinear transition. Use finite, physically meaningful values.",
    ),
    (
        "introducing multi-decade component scale separation",
        "Change selected resistances, currents, voltages, or device geometries across several orders of magnitude while keeping values finite and positive where required. Do not create an open circuit or an ideal-source contradiction.",
    ),
    (
        "weakening DC bias and leakage paths without floating nodes",
        "Increase selected shunt or bias resistances, or reduce selected bias currents, to make node restoration weak. Every node must still retain a finite DC path to ground or a supply.",
    ),
    (
        "strengthening interacting source and bias drives",
        "Increase or rebalance supported independent DC voltage/current sources so multiple nonlinear devices strongly interact. Avoid parallel unequal ideal voltage sources and ideal voltage-source loops.",
    ),
    (
        "reducing a stabilizing negative-feedback path",
        "Weaken or remove one physical resistive negative-feedback path while keeping the circuit well posed. Do not remove the last DC path of any node and do not use solver-control options.",
    ),
    (
        "making the operating point near a device threshold",
        "Adjust DC biases and supported model thresholds so one or more MOSFETs, BJTs, or diodes operate close to turn-on. Retain finite bias paths and valid model/instance syntax.",
    ),
    (
        "perturbing DC initial guesses without constraining the final solution",
        "Add, remove, or change .nodeset or .ic voltage hints using existing nodes. Keep hints finite and non-contradictory; do not clamp nodes with new ideal sources and do not change solver options.",
    ),
    (
        "creating competing DC equilibria with a slight branch-selection bias",
        "Create a supported bistable or multistable nonlinear topology, then add a weak resistor, small source, .nodeset, or .ic hint that selects a branch without eliminating the competing equilibria.",
    ),
    (
        "increasing nonlinear fanout and node coupling",
        "Connect additional supported transistor control terminals or diode branches to an existing high-gain node. Keep loading finite, preserve polarity, and ensure every new node has a DC reference path.",
    ),
)


def initialize_pta_mutations():
    mutations = initialize_mutations()
    base = [mutations[index] for index in PTA_MUTATION_INDICES]
    return base + [description for description, _ in PTA_DIRECTED_MUTATION_SPECS]


def initialize_pta_forms():
    return list(PTA_BASE_FORMS) + [
        guidance for _, guidance in PTA_DIRECTED_MUTATION_SPECS
    ]
